#include "ReverbEngine.h"

#include <cmath>

namespace
{
    // Equal-power crossfade gains for the Early/Late Balance in the FDN
    // modes - the same law the procedural generator already bakes into the
    // Classic path, so the knob means the same thing in every mode.
    void equalPowerBalance (float balance01, float& earlyGain, float& lateGain) noexcept
    {
        const auto clamped = juce::jlimit (0.0f, 1.0f, balance01);
        earlyGain = std::cos (clamped * juce::MathConstants<float>::halfPi);
        lateGain = std::sin (clamped * juce::MathConstants<float>::halfPi);
    }
}

//==============================================================================
bool ReverbEngine::RenderRequest::operator== (const RenderRequest& other) const noexcept
{
    constexpr float epsilon = 1.0e-4f;

    return std::abs (decaySeconds - other.decaySeconds) < epsilon
            && std::abs (dampingHz - other.dampingHz) < epsilon
            && space == other.space
            && std::abs (earlyLateBalance01 - other.earlyLateBalance01) < epsilon
            && freeze == other.freeze
            && std::abs (size01 - other.size01) < epsilon
            && std::abs (bassDecayMultiplier - other.bassDecayMultiplier) < epsilon
            && engineMode == other.engineMode
            && userIr == other.userIr
            && userIrFile == other.userIrFile;
}

//==============================================================================
ReverbEngine::ReverbEngine()
{
    userIrFormatManager.registerBasicFormats();

    // Low priority: a late kernel means a knob takes a few more milliseconds
    // to take effect, whereas competing with the audio thread for a core means
    // a dropout.
    renderThread.startThread (juce::Thread::Priority::low);
}

ReverbEngine::~ReverbEngine()
{
    renderThread.signalThreadShouldExit();
    renderWakeUp.signal();
    renderThread.stopThread (2000);
}

//==============================================================================
void ReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    // See the class-level THREADING comment (ReverbEngine.h): the host may
    // call prepareToPlay() -> this method from any non-audio thread, and
    // the background "Requiem IR Render" thread's renderOnce() call reads/
    // writes the same sampleRate/numChannels/fdnTail state this method
    // mutates below - both must be serialised against each other.
    const std::lock_guard<std::recursive_mutex> reconfigureLock (reconfigureMutex);

    sampleRate = spec.sampleRate;
    numChannels = static_cast<int> (spec.numChannels);
    maximumBlockSize = static_cast<int> (spec.maximumBlockSize);

    // Discard anything a previous session's render queued but process() never
    // consumed - it was sized and sampled for the old rate.
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse = PendingImpulseResponse {};
    }

    {
        const juce::SpinLock::ScopedLockType lock (pendingHybridSetupLock);
        hasPendingHybridSetup = false;
    }

    designContext.prepare (sampleRate);

    // The FDN has to be prepared before the first render: the render needs its
    // delay-line lengths to fit the attenuation and to work out the branch's
    // onset compensation.
    fdnTail.prepare (spec);

    // Render the initial kernel and install it *before* the convolution
    // engines are prepared. juce::dsp::Convolution documents that
    // loadImpulseResponse() is asynchronous but that prepare() forces the most
    // recently loaded IR to be fully initialised, and therefore that
    // "it is recommended to call loadImpulseResponse() *before* prepare() if a
    // specific IR must be active during the first process() call". Loading
    // afterwards leaves the first blocks running against an empty engine -
    // silence in an offline render, which is fast enough that the background
    // loader never gets a chance to catch up.
    {
        const auto request = buildRequest();
        renderOnce (request);

        const juce::ScopedLock lock (requestLock);
        requestedRender = request;
        lastRenderedRequest = request;
        hasRenderedOnce = true;
    }

    if (usingUserImpulseResponse && userImpulseResponseFile.existsAsFile())
    {
        morphingConvolution.loadFileSynchronously (userImpulseResponseFile, numChannels);

        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse.kind = PendingImpulseResponseKind::none;
    }
    else
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);

        if (pendingImpulseResponse.kind == PendingImpulseResponseKind::procedural)
        {
            lastConsumedPadSamples.store (pendingImpulseResponse.padSamples, std::memory_order_relaxed);
            morphingConvolution.loadKernelSynchronously (std::move (pendingImpulseResponse.proceduralBuffer),
                                                          pendingImpulseResponse.proceduralSampleRate, numChannels);
        }

        pendingImpulseResponse.kind = PendingImpulseResponseKind::none;
    }

    morphingConvolution.prepare (spec);
    preDelayLine.prepare (spec);
    branchDelayLine.prepare (spec);
    wetChain.prepare (spec);

    branchBuffer.setSize (juce::jmax (2, numChannels), juce::jmax (1, maximumBlockSize), false, true, true);
    dryMonoBuffer.setSize (1, juce::jmax (1, maximumBlockSize), false, true, true);

    correctionFirState.assign (static_cast<size_t> (juce::jmax (2, numChannels)),
                                std::array<float, IrAnalysis::correctionFirLength> {});
    correctionFirPositions.assign (static_cast<size_t> (juce::jmax (2, numChannels)), 0);

    // See docs/architecture.md ("DryWetMixer gotcha"): juce::dsp::Chorus owns
    // its own internal DryWetMixer, primed the same way - configuring its
    // parameters before prepare() means the first block already reflects
    // lastModulationAmount01 rather than the class's own defaults.
    modulationChorus.setRate (modulationRateHz);
    modulationChorus.setCentreDelay (modulationCentreDelayMs);
    modulationChorus.setFeedback (0.0f);
    modulationChorus.setDepth (mapModulationDepth (lastModulationAmount01));
    modulationChorus.setMix (mapModulationMix (lastModulationAmount01));
    modulationChorus.prepare (spec);

    outputGain.setRampDurationSeconds (smoothingTimeSeconds);
    outputGain.prepare (spec);

    dryWetMixer.prepare (spec);

    // Both convolution engines are zero-latency/uniformly partitioned, so this
    // is 0 - queried rather than assumed so the plugin stays correct if a
    // fixed-latency configuration is ever adopted.
    latencySamples = morphingConvolution.getLatency();
    dryWetMixer.setWetLatency (static_cast<float> (latencySamples));

    // See docs/architecture.md ("DryWetMixer gotcha"): priming the real target
    // before reset() means the mixer starts at the correct balance instead of
    // ramping up from fully wet over its internal ~50 ms default.
    dryWetMixer.setWetMixProportion (lastMixProportion);

    preDelayMsSmoothed.reset (sampleRate, smoothingTimeSeconds);
    preDelayMsSmoothed.setCurrentAndTargetValue (lastPreDelayMs);
    widthAmountSmoothed.reset (sampleRate, smoothingTimeSeconds);
    widthAmountSmoothed.setCurrentAndTargetValue (lastWidthPercent * 0.01f);
    branchGainSmoothed.reset (sampleRate, modeCrossfadeSeconds);
    branchGainSmoothed.setCurrentAndTargetValue (0.0f);

    applyPendingHybridSetupIfAny();

    reset();

    // reset() clears buffer/position state but knows nothing about
    // lastPreDelayMs, so prime the delay line's sample count directly.
    const auto preDelaySamples = juce::jlimit (0.0f,
                                                static_cast<float> (preDelayLine.getMaximumDelayInSamples()),
                                                lastPreDelayMs * 0.001f * static_cast<float> (sampleRate));
    preDelayLine.setDelay (preDelaySamples);
    branchDelayLine.setDelay (static_cast<float> (juce::jlimit (0, maxBranchDelaySamples - 1, hybridBranchDelaySamples)));
}

void ReverbEngine::reset()
{
    preDelayLine.reset();
    branchDelayLine.reset();
    morphingConvolution.reset();
    modulationChorus.reset();
    outputGain.reset();
    dryWetMixer.reset();
    fdnTail.reset();
    wetChain.reset();

    branchBuffer.clear();
    dryMonoBuffer.clear();

    for (auto& state : correctionFirState)
        state.fill (0.0f);

    for (auto& position : correctionFirPositions)
        position = 0;
}

//==============================================================================
void ReverbEngine::setPreDelayMs (float newPreDelayMs)
{
    lastPreDelayMs = newPreDelayMs;
    preDelayMsSmoothed.setTargetValue (newPreDelayMs);
}

void ReverbEngine::setWidthPercent (float newWidthPercent)
{
    lastWidthPercent = newWidthPercent;
    widthAmountSmoothed.setTargetValue (newWidthPercent * 0.01f);
}

void ReverbEngine::setMixProportion (float newProportion01)
{
    lastMixProportion = newProportion01;
    dryWetMixer.setWetMixProportion (newProportion01);
}

void ReverbEngine::setOutputDb (float newOutputDb)
{
    outputGain.setGainDecibels (newOutputDb);
}

float ReverbEngine::mapModulationDepth (float amount01) noexcept
{
    return juce::jmap (juce::jlimit (0.0f, 1.0f, amount01), 0.0f, 1.0f, 0.05f, 0.35f);
}

float ReverbEngine::mapModulationMix (float amount01) noexcept
{
    return juce::jlimit (0.0f, 1.0f, amount01) * 0.5f;
}

void ReverbEngine::setModulationAmount (float newAmount01)
{
    lastModulationAmount01 = juce::jlimit (0.0f, 1.0f, newAmount01);
    modulationChorus.setDepth (mapModulationDepth (lastModulationAmount01));
    modulationChorus.setMix (mapModulationMix (lastModulationAmount01));
}

void ReverbEngine::setDecaySeconds (float newDecaySeconds)
{
    requestedDecaySeconds.store (newDecaySeconds, std::memory_order_relaxed);
}

void ReverbEngine::setDampingHz (float newDampingHz)
{
    requestedDampingHz.store (newDampingHz, std::memory_order_relaxed);
}

void ReverbEngine::setSpaceType (ReverbIR::SpaceType newSpace)
{
    requestedSpace.store (static_cast<int> (newSpace), std::memory_order_relaxed);
}

void ReverbEngine::setEarlyLateBalance (float newBalance01)
{
    requestedEarlyLateBalance01.store (newBalance01, std::memory_order_relaxed);
}

void ReverbEngine::setFreeze (bool shouldFreeze)
{
    requestedFreeze.store (shouldFreeze, std::memory_order_relaxed);

    // In the FDN modes Freeze is structural and instant: the network's
    // attenuation is crossfaded out to unity right here, on the audio thread,
    // with no impulse-response regeneration and no dependency on the 20 Hz
    // timer. Classic mode keeps the legacy flat-envelope IR freeze, which is
    // why the atomic above is still set unconditionally.
    fdnTail.setFrozen (shouldFreeze);
}

void ReverbEngine::setSize (float newSize01)
{
    requestedSize01.store (newSize01, std::memory_order_relaxed);
}

void ReverbEngine::setBassDecayMultiplier (float newBassDecayMultiplier)
{
    requestedBassDecayMultiplier.store (newBassDecayMultiplier, std::memory_order_relaxed);
}

//==============================================================================
void ReverbEngine::setEngineMode (EngineMode newMode)
{
    requestedEngineMode.store (static_cast<int> (newMode), std::memory_order_relaxed);
}

void ReverbEngine::setTailModMode (FdnTail::ModulationMode newMode)
{
    fdnTail.setModulationMode (newMode);
}

void ReverbEngine::setTailModDepth (float newDepth01)
{
    fdnTail.setModulationDepth (newDepth01);
}

void ReverbEngine::setTailModRateScale (float newScale)
{
    fdnTail.setModulationRateScale (newScale);
}

void ReverbEngine::setBloomAmount (float newAmount01)
{
    requestedBloomAmount01.store (juce::jlimit (0.0f, 1.0f, newAmount01), std::memory_order_relaxed);
}

void ReverbEngine::setLowCutHz (float newLowCutHz)        { wetChain.setLowCutHz (newLowCutHz); }
void ReverbEngine::setHighCutHz (float newHighCutHz)      { wetChain.setHighCutHz (newHighCutHz); }
void ReverbEngine::setDuckAmountPercent (float newAmount) { wetChain.setDuckAmountPercent (newAmount); }
void ReverbEngine::setDuckAttackMs (float newAttackMs)    { wetChain.setDuckAttackMs (newAttackMs); }
void ReverbEngine::setDuckReleaseMs (float newReleaseMs)  { wetChain.setDuckReleaseMs (newReleaseMs); }

//==============================================================================
ReverbEngine::RenderRequest ReverbEngine::buildRequest() const
{
    // usingUserImpulseResponse/userImpulseResponseFile below are plain,
    // unsynchronised members written by loadUserImpulseResponse()/
    // clearUserImpulseResponse() from either the message thread (GUI) or
    // the host thread (setStateInformation()) - see the class-level
    // THREADING comment. reconfigureMutex is recursive, so this is safe to
    // call both directly (regenerateImpulseResponseIfNeeded(), the message
    // thread) and from callers that already hold the lock (prepare(),
    // loadUserImpulseResponse(), clearUserImpulseResponse()).
    const std::lock_guard<std::recursive_mutex> reconfigureLock (reconfigureMutex);

    RenderRequest request;
    request.decaySeconds = requestedDecaySeconds.load (std::memory_order_relaxed);
    request.dampingHz = requestedDampingHz.load (std::memory_order_relaxed);
    request.space = requestedSpace.load (std::memory_order_relaxed);
    request.earlyLateBalance01 = requestedEarlyLateBalance01.load (std::memory_order_relaxed);
    request.freeze = requestedFreeze.load (std::memory_order_relaxed);
    request.size01 = requestedSize01.load (std::memory_order_relaxed);
    request.bassDecayMultiplier = requestedBassDecayMultiplier.load (std::memory_order_relaxed);
    request.engineMode = requestedEngineMode.load (std::memory_order_relaxed);
    request.userIr = usingUserImpulseResponse;
    request.userIrFile = userImpulseResponseFile;
    return request;
}

void ReverbEngine::postRequest (const RenderRequest& request)
{
    {
        const juce::ScopedLock lock (requestLock);
        requestedRender = request;
    }

    renderCompleted.reset();
    renderWakeUp.signal();
}

void ReverbEngine::regenerateImpulseResponseIfNeeded()
{
    const auto request = buildRequest();

    {
        const juce::ScopedLock lock (requestLock);

        if (hasRenderedOnce && request == lastRenderedRequest && request == requestedRender)
            return;
    }

    postRequest (request);
}

bool ReverbEngine::waitForPendingRender (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (juce::jmax (0, timeoutMs));

    for (;;)
    {
        {
            const juce::ScopedLock lock (requestLock);

            if (hasRenderedOnce && lastRenderedRequest == requestedRender)
                return true;
        }

        const auto now = juce::Time::getMillisecondCounter();

        if (now >= deadline)
            return false;

        renderCompleted.wait (static_cast<int> (juce::jmin (static_cast<juce::uint32> (25), deadline - now)));
    }
}

void ReverbEngine::runRenderLoop()
{
    while (! renderThread.currentThreadShouldExit())
    {
        renderWakeUp.wait (200);

        if (renderThread.currentThreadShouldExit())
            return;

        for (;;)
        {
            RenderRequest request;

            {
                const juce::ScopedLock lock (requestLock);

                if (hasRenderedOnce && requestedRender == lastRenderedRequest)
                    break;

                // Coalescing: whatever is in the slot right now is the job.
                // Anything that arrived while the previous render was running
                // has already overwritten it, so there is never more than one
                // outstanding job and stale ones are simply never seen.
                request = requestedRender;
            }

            // Deliberately NOT held across the requestLock-protected block
            // above/below - only the render call itself needs to be
            // mutually exclusive with prepare() (see the class-level
            // THREADING comment), which reads/writes the very same
            // sampleRate/numChannels/fdnTail state renderOnce() does.
            {
                const std::lock_guard<std::recursive_mutex> reconfigureLock (reconfigureMutex);
                renderOnce (request);
            }

            {
                const juce::ScopedLock lock (requestLock);
                lastRenderedRequest = request;
                hasRenderedOnce = true;
            }
        }

        renderCompleted.signal();
    }
}

//==============================================================================
void ReverbEngine::renderOnce (const RenderRequest& request)
{
    const auto mode = static_cast<EngineMode> (request.engineMode);
    const auto usesFdn = mode != EngineMode::classicConvolution;

    //==========================================================================
    // A user impulse response is loaded by juce::dsp::Convolution itself, so
    // there is no kernel to render here - but the FDN modes still need the
    // file analysed, which costs a decode. That is exactly why this runs on a
    // background thread.
    juce::AudioBuffer<float> impulseResponse;
    auto haveBuffer = false;

    if (request.userIr)
    {
        if (usesFdn && request.userIrFile.existsAsFile())
        {
            std::unique_ptr<juce::AudioFormatReader> reader (userIrFormatManager.createReaderFor (request.userIrFile));

            if (reader != nullptr && reader->numChannels > 0 && reader->lengthInSamples > 0 && reader->sampleRate > 0.0)
            {
                const auto maxSamples = static_cast<juce::int64> (maxUserImpulseResponseSeconds * reader->sampleRate);
                const auto lengthToRead = static_cast<int> (juce::jmin (reader->lengthInSamples, maxSamples));

                impulseResponse.setSize (juce::jmin (2, static_cast<int> (reader->numChannels)), lengthToRead);
                reader->read (&impulseResponse, 0, lengthToRead, 0, true, impulseResponse.getNumChannels() > 1);
                haveBuffer = true;
            }
        }
    }
    else
    {
        // Freeze in the FDN modes is structural - the network holds the audio
        // already circulating in it - so the *kernel* keeps its normal shape.
        // Baking the generator's flat-envelope freeze into the early field as
        // well would sustain the early reflections too, which is not what a
        // frozen tail sounds like.
        const auto generatorFreeze = usesFdn ? false : request.freeze;

        impulseResponse = ReverbIR::generateProceduralImpulseResponse (
            sampleRate, request.decaySeconds, request.dampingHz, numChannels,
            static_cast<ReverbIR::SpaceType> (request.space), request.earlyLateBalance01,
            generatorFreeze, 1, request.size01, request.bassDecayMultiplier);

        haveBuffer = true;
    }

    //==========================================================================
    // Analysis and FDN fit.
    HybridSetup setup;

    if (usesFdn && haveBuffer && impulseResponse.getNumSamples() > 0)
    {
        const auto analysis = request.userIr
                                 ? IrAnalysis::analyse (impulseResponse, sampleRate)
                                 : IrAnalysis::analyseProcedural (impulseResponse, sampleRate,
                                                                   request.decaySeconds, request.dampingHz,
                                                                   request.bassDecayMultiplier);

        setup.valid = true;
        setup.mixingTimeSeconds = analysis.mixingTimeSeconds;
        setup.lowConfidence = analysis.hasLowConfidence;

        // Fit every delay line's attenuation to the analysed decay curve. A
        // continuous Decay drag is only this re-solve - the tail's decay rate
        // changes smoothly, with no kernel reload and therefore no staircase.
        std::array<AttenuationDesign::LineAttenuation, FdnTail::numLines> attenuation {};

        for (int line = 0; line < FdnTail::numLines; ++line)
            attenuation[static_cast<size_t> (line)] =
                designContext.design (analysis.rt60Octave, fdnTail.getDelayLengths()[static_cast<size_t> (line)]);

        fdnTail.postAttenuation (attenuation);

        //======================================================================
        // Correction FIR (Carpentier et al., DAFx-14, eq. 3). The impulse
        // response's residual band energy from t_mix onward is compared with
        // the energy the FDN will itself produce from that instant, and the
        // FIR corrects the difference, so the synthesised tail picks up with
        // the spectrum the truncated convolution handed over.
        //
        // The FDN's own residual energy is taken analytically from the decay
        // curve it has just been fitted to - for an exponential decay with
        // amplitude constant beta = ln(1000)/T60, the energy remaining from
        // its onset is proportional to 1/(2 beta) - rather than measured by
        // rendering the network offline. That is what keeps a Decay drag down
        // to a re-solve; the cost is that the absolute level match is good to
        // a couple of dB rather than exact, and the overall level is set by
        // the Early/Late Balance anyway.
        std::array<float, IrAnalysis::numOctaveBands> perBandGains {};
        perBandGains.fill (1.0f);

        const auto centres = IrAnalysis::octaveCentreFrequencies();
        auto usableBands = 0;
        auto logSum = 0.0f;

        for (int band = 0; band < IrAnalysis::numOctaveBands; ++band)
        {
            if (! IrAnalysis::isBandUsable (centres[static_cast<size_t> (band)], sampleRate))
                continue;

            const auto t60 = juce::jmax (0.05f, analysis.rt60Octave[static_cast<size_t> (band)]);
            const auto beta = 6.90775528f / t60;
            const auto fdnEnergy = 1.0f / (2.0f * beta);
            const auto irEnergy = juce::jmax (1.0e-20f, analysis.edrAtMixingTime[static_cast<size_t> (band)]);

            const auto gain = std::sqrt (irEnergy / fdnEnergy);
            perBandGains[static_cast<size_t> (band)] = gain;

            logSum += std::log (juce::jmax (1.0e-12f, gain));
            ++usableBands;
        }

        // Normalise to unit geometric mean: the FIR shapes the tail's spectral
        // tilt, it does not set its level.
        if (usableBands > 0)
        {
            const auto normalisation = std::exp (logSum / static_cast<float> (usableBands));

            if (normalisation > 0.0f)
                for (auto& gain : perBandGains)
                    gain /= normalisation;
        }

        setup.correctionFir = IrAnalysis::designCorrectionFir (perBandGains, sampleRate);
        setup.correctionFirActive = true;

        //======================================================================
        // Branch onset compensation (brief 3.2). The FDN's output reads only
        // delay-line outputs - there is no direct feedthrough - so it emits
        // nothing until its shortest delay line has run, and the correction
        // FIR adds its own 128-sample group delay on top. BOTH have to come
        // out of the branch pre-delay. Subtracting only the FIR's 128 samples
        // would put the tail's onset about 34 ms late at 48 kHz: an audible
        // energy hole immediately after the early field's 10 ms fade.
        const auto mixingSamples = static_cast<int> (std::round (setup.mixingTimeSeconds * sampleRate));
        const auto intrinsicOnset = IrAnalysis::correctionFirGroupDelay + fdnTail.getShortestDelaySamples();

        setup.branchDelaySamples = juce::jlimit (0, maxBranchDelaySamples - 1, mixingSamples - intrinsicOnset);
    }

    //==========================================================================
    // Truncate the kernel at the mixing time in Hybrid mode. A low-confidence
    // analysis - a gated or chopped impulse response, whose decay is nothing
    // like exponential - keeps the full kernel instead: splicing a fitted
    // exponential tail onto a decay that is not exponential sounds worse than
    // not splicing at all.
    const auto spliceKernel = mode == EngineMode::hybridTail && setup.valid && ! setup.lowConfidence;

    if (spliceKernel && haveBuffer)
    {
        for (int channel = 0; channel < impulseResponse.getNumChannels(); ++channel)
        {
            auto* data = impulseResponse.getWritePointer (channel);

            for (int i = 0; i < impulseResponse.getNumSamples(); ++i)
                data[i] *= IrAnalysis::spliceWindow (i, sampleRate, setup.mixingTimeSeconds);
        }

        // Drop the samples past the fade - they are all zero now, and a kernel
        // an order of magnitude shorter is the whole CPU argument for Hybrid.
        const auto fadeEnd = static_cast<int> (std::round ((setup.mixingTimeSeconds + IrAnalysis::spliceFadeSeconds)
                                                             * sampleRate)) + 2;

        if (fadeEnd < impulseResponse.getNumSamples())
            impulseResponse.setSize (impulseResponse.getNumChannels(), fadeEnd, true, true, true);
    }

    //==========================================================================
    // Hand the results over.
    {
        const juce::SpinLock::ScopedLockType lock (pendingHybridSetupLock);
        pendingHybridSetup = setup;
        hasPendingHybridSetup = true;
    }

    if (request.userIr)
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse.kind = PendingImpulseResponseKind::userFile;
        pendingImpulseResponse.userFile = request.userIrFile;
        pendingImpulseResponse.padSamples = 0;
        return;
    }

    if (! haveBuffer)
        return;

    // Alternating trailing zero pad. MorphingConvolution's readiness handshake
    // uses the posted kernel's length as its install sentinel, and a
    // Damping-only change regenerates an impulse response of exactly the same
    // length - so without this the handshake could never tell "installed" from
    // "still holding the old one". Derived from the pad the audio thread last
    // *consumed*, so a kernel coalesced away before consumption cannot break
    // the alternation. Applied here, off the audio thread, because growing an
    // AudioBuffer allocates.
    const auto pad = 1 - lastConsumedPadSamples.load (std::memory_order_relaxed);

    if (pad > 0)
        impulseResponse.setSize (impulseResponse.getNumChannels(),
                                  impulseResponse.getNumSamples() + pad, true, true, true);

    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse.kind = PendingImpulseResponseKind::procedural;
        pendingImpulseResponse.proceduralBuffer = std::move (impulseResponse);
        pendingImpulseResponse.proceduralSampleRate = sampleRate;
        pendingImpulseResponse.padSamples = pad;
    }
}

//==============================================================================
bool ReverbEngine::loadUserImpulseResponse (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    // Sanity-check that the file really is readable audio (and not
    // pathologically long) before anything else changes, so a bogus or
    // mis-selected file leaves the currently active impulse response - and
    // therefore what the plugin is producing - completely untouched. Done
    // outside reconfigureMutex: it is pure file I/O with no shared-state
    // access, and holding the lock across a filesystem read would needlessly
    // block prepare()/renderOnce() for its duration.
    std::unique_ptr<juce::AudioFormatReader> reader (userIrFormatManager.createReaderFor (file));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const auto durationSeconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    reader.reset(); // release it before Convolution opens the file itself

    if (durationSeconds > maxUserImpulseResponseSeconds)
        return false;

    // usingUserImpulseResponse/userImpulseResponseFile are read by prepare()
    // (host thread) and buildRequest() (any of prepare()/
    // regenerateImpulseResponseIfNeeded()/this method's own caller) - see
    // the class-level THREADING comment. This method's own two real callers
    // (RequiemAudioProcessor::loadUserImpulseResponseFile(), message thread,
    // and setStateInformation(), host thread) are exactly the two
    // unsynchronised non-audio threads that bug class is about.
    const std::lock_guard<std::recursive_mutex> reconfigureLock (reconfigureMutex);

    usingUserImpulseResponse = true;
    userImpulseResponseFile = file;

    postRequest (buildRequest());
    return true;
}

void ReverbEngine::clearUserImpulseResponse()
{
    const std::lock_guard<std::recursive_mutex> reconfigureLock (reconfigureMutex);

    if (! usingUserImpulseResponse)
        return;

    usingUserImpulseResponse = false;
    userImpulseResponseFile = juce::File();

    postRequest (buildRequest());
}

//==============================================================================
void ReverbEngine::applyPendingImpulseResponseIfAny() noexcept
{
    // Leave the kernel in the slot while a morph is still in flight rather
    // than dropping it or buffering a second one here - buffering would mean
    // freeing an AudioBuffer on the audio thread.
    if (! morphingConvolution.canAcceptKernel())
        return;

    const juce::SpinLock::ScopedTryLockType lock (pendingImpulseResponseLock);

    if (! lock.isLocked() || pendingImpulseResponse.kind == PendingImpulseResponseKind::none)
        return;

    if (pendingImpulseResponse.kind == PendingImpulseResponseKind::procedural)
    {
        lastConsumedPadSamples.store (pendingImpulseResponse.padSamples, std::memory_order_relaxed);

        morphingConvolution.postKernel (std::move (pendingImpulseResponse.proceduralBuffer),
                                         pendingImpulseResponse.proceduralSampleRate, numChannels);
    }
    else
    {
        morphingConvolution.postFile (pendingImpulseResponse.userFile, numChannels);
    }

    pendingImpulseResponse.kind = PendingImpulseResponseKind::none;
}

void ReverbEngine::applyPendingHybridSetupIfAny() noexcept
{
    const juce::SpinLock::ScopedTryLockType lock (pendingHybridSetupLock);

    if (! lock.isLocked() || ! hasPendingHybridSetup)
        return;

    if (pendingHybridSetup.valid)
    {
        activeMixingTimeSeconds = pendingHybridSetup.mixingTimeSeconds;
        hybridBranchDelaySamples = pendingHybridSetup.branchDelaySamples;
        lowAnalysisConfidence = pendingHybridSetup.lowConfidence;
        correctionFirTaps = pendingHybridSetup.correctionFir;
        correctionFirActive = pendingHybridSetup.correctionFirActive;
    }

    hasPendingHybridSetup = false;
}

//==============================================================================
void ReverbEngine::processCorrectionFir (juce::dsp::AudioBlock<float>& block, int numSamples) noexcept
{
    if (! correctionFirActive)
        return;

    const auto blockChannels = static_cast<int> (block.getNumChannels());
    const auto usableChannels = juce::jmin (blockChannels, static_cast<int> (correctionFirState.size()));

    for (int channel = 0; channel < usableChannels; ++channel)
    {
        auto* data = block.getChannelPointer (static_cast<size_t> (channel));
        auto& state = correctionFirState[static_cast<size_t> (channel)];
        auto& position = correctionFirPositions[static_cast<size_t> (channel)];

        for (int i = 0; i < numSamples; ++i)
        {
            state[static_cast<size_t> (position)] = data[i];

            auto accumulator = 0.0f;
            auto readIndex = position;

            for (int tap = 0; tap < IrAnalysis::correctionFirLength; ++tap)
            {
                accumulator += correctionFirTaps[static_cast<size_t> (tap)] * state[static_cast<size_t> (readIndex)];

                if (--readIndex < 0)
                    readIndex = IrAnalysis::correctionFirLength - 1;
            }

            data[i] = accumulator;

            if (++position >= IrAnalysis::correctionFirLength)
                position = 0;
        }
    }
}

//==============================================================================
void ReverbEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = static_cast<int> (block.getNumSamples());

    if (numSamples <= 0)
        return;

    applyPendingImpulseResponseIfAny();
    applyPendingHybridSetupIfAny();

    const auto blockChannels = static_cast<int> (block.getNumChannels());
    const auto mode = static_cast<EngineMode> (requestedEngineMode.load (std::memory_order_relaxed));
    const auto usesFdn = mode != EngineMode::classicConvolution;
    const auto scratchUsable = numSamples <= branchBuffer.getNumSamples()
                                && blockChannels <= branchBuffer.getNumChannels()
                                && numSamples <= dryMonoBuffer.getNumSamples();

    //==========================================================================
    // Capture the pre-everything dry signal: DryWetMixer needs it for the mix
    // (delayed internally by the reported latency so it stays time-aligned),
    // and the ducker needs a mono sum of it as its sidechain. Pre-Delay is
    // deliberately *not* part of that compensation - it is an audible effect
    // parameter, not something to hide from the dry signal.
    dryWetMixer.pushDrySamples (block);

    if (scratchUsable)
    {
        auto* mono = dryMonoBuffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
        {
            auto sum = 0.0f;

            for (int channel = 0; channel < blockChannels; ++channel)
                sum += block.getChannelPointer (static_cast<size_t> (channel))[i];

            mono[i] = sum / static_cast<float> (juce::jmax (1, blockChannels));
        }
    }

    const auto preDelaySamples = juce::jlimit (0.0f,
                                                static_cast<float> (preDelayLine.getMaximumDelayInSamples()),
                                                preDelayMsSmoothed.skip (numSamples) * 0.001f * static_cast<float> (sampleRate));
    preDelayLine.setDelay (preDelaySamples);

    juce::dsp::ProcessContextReplacing<float> context (block);
    preDelayLine.process (context);

    //==========================================================================
    // FDN branch. Snapshot the pre-delayed input before the convolution
    // overwrites the block in place, then run pre-delay -> FDN -> correction
    // FIR on the copy.
    auto runFdnBranch = usesFdn && scratchUsable;

    // In Hybrid mode a low-confidence analysis keeps the full convolution and
    // suppresses the synthesised tail (brief section 7, risk 2).
    if (mode == EngineMode::hybridTail && lowAnalysisConfidence)
        runFdnBranch = false;

    float earlyGain = 1.0f;
    float lateGain = 0.0f;
    equalPowerBalance (requestedEarlyLateBalance01.load (std::memory_order_relaxed), earlyGain, lateGain);

    // Bloom's branch gain is the square of the normalised amount: a soft taper
    // that reaches silence at 0% and unity at 100%, so the low end of the knob
    // is usable rather than jumping straight to a fully audible layer.
    const auto bloomAmount = requestedBloomAmount01.load (std::memory_order_relaxed);
    const auto targetBranchGain = ! runFdnBranch ? 0.0f
                                    : mode == EngineMode::tailBloom ? bloomAmount * bloomAmount
                                                                     : lateGain;

    branchGainSmoothed.setTargetValue (targetBranchGain);

    if (runFdnBranch)
    {
        for (int channel = 0; channel < blockChannels; ++channel)
            branchBuffer.copyFrom (channel, 0, block.getChannelPointer (static_cast<size_t> (channel)), numSamples);

        auto branchBlock = juce::dsp::AudioBlock<float> (branchBuffer)
                               .getSubBlock (0, static_cast<size_t> (numSamples))
                               .getSubsetChannelBlock (0, static_cast<size_t> (blockChannels));

        // Bloom pre-delays by 0.6 of the mixing time: the bloom layer swells
        // up underneath a capture that is already playing, rather than
        // replacing its tail.
        const auto branchDelay = mode == EngineMode::tailBloom
                                    ? 0.6f * activeMixingTimeSeconds * static_cast<float> (sampleRate)
                                    : static_cast<float> (hybridBranchDelaySamples);

        branchDelayLine.setDelay (juce::jlimit (0.0f, static_cast<float> (maxBranchDelaySamples - 1), branchDelay));

        juce::dsp::ProcessContextReplacing<float> branchContext (branchBlock);
        branchDelayLine.process (branchContext);

        fdnTail.process (branchBlock);
        processCorrectionFir (branchBlock, numSamples);
    }

    //==========================================================================
    // Convolution. In Hybrid mode the kernel is already the truncated early
    // field, so the Early/Late Balance is all that is applied on top.
    morphingConvolution.process (block);

    if (runFdnBranch && mode == EngineMode::hybridTail)
        block.multiplyBy (earlyGain);

    //==========================================================================
    // Sum the branch in.
    if (runFdnBranch)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = branchGainSmoothed.getNextValue();

            for (int channel = 0; channel < blockChannels; ++channel)
                block.getChannelPointer (static_cast<size_t> (channel))[i] +=
                    gain * branchBuffer.getSample (channel, i);
        }
    }
    else
    {
        branchGainSmoothed.skip (numSamples);
    }

    modulationChorus.process (context);

    applyWidth (block);

    wetChain.process (block, scratchUsable ? dryMonoBuffer.getReadPointer (0) : nullptr, numSamples);

    dryWetMixer.mixWetSamples (block);

    outputGain.process (context);
}

void ReverbEngine::applyWidth (juce::dsp::AudioBlock<float>& block) noexcept
{
    if (block.getNumChannels() != 2)
        return; // Width is only meaningful for a stereo wet signal.

    const auto numSamples = block.getNumSamples();
    const auto width = widthAmountSmoothed.skip (static_cast<int> (numSamples));

    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);

    for (size_t i = 0; i < numSamples; ++i)
    {
        const auto mid = 0.5f * (left[i] + right[i]);
        const auto side = 0.5f * (left[i] - right[i]) * width;

        left[i] = mid + side;
        right[i] = mid - side;
    }
}
