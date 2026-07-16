#include "ReverbEngine.h"

ReverbEngine::ReverbEngine()
{
    userIrFormatManager.registerBasicFormats();
}

void ReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = static_cast<int> (spec.numChannels);

    // Discard any request queued by a previous session's Timer/FileChooser
    // callback before it could be applied (e.g. a sample-rate change came
    // in first) - a stale procedural buffer would be sized/sampled for the
    // old rate, and process() hasn't run since to have consumed it anyway.
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse = PendingImpulseResponse {};
    }

    const auto stereo = numChannels >= 2 ? juce::dsp::Convolution::Stereo::yes
                                          : juce::dsp::Convolution::Stereo::no;

    // Load the active impulse response (procedural or user override) before
    // convolution.prepare(), per juce::dsp::Convolution's own recommended
    // ordering ("it is recommended to call loadImpulseResponse() *before*
    // prepare() if a specific IR must be active during the first process()
    // call") - otherwise the very first block after prepare() could run
    // against whatever IR (if any) happened to be active previously. Called
    // directly (not queued): prepare() only ever runs from prepareToPlay(),
    // with the host contractually guaranteeing processing is suspended, so
    // there is no concurrent process() call for a direct
    // loadImpulseResponse() call here to race (see the class comment's
    // "Threading" section).
    if (usingUserImpulseResponse && userImpulseResponseFile.existsAsFile())
    {
        convolution.loadImpulseResponse (userImpulseResponseFile,
                                          stereo,
                                          juce::dsp::Convolution::Trim::yes,
                                          0,
                                          juce::dsp::Convolution::Normalise::yes);
    }
    else
    {
        // Always (re)generate here, even if the requested parameters happen
        // to match the last-generated ones: prepare() can run after a
        // sample-rate change, which invalidates any previously generated
        // buffer (it was sized and sampled for the old rate).
        loadProceduralImpulseResponseSynchronously (requestedDecaySeconds.load (std::memory_order_relaxed),
                                                      requestedDampingHz.load (std::memory_order_relaxed),
                                                      static_cast<ReverbIR::SpaceType> (requestedSpace.load (std::memory_order_relaxed)),
                                                      requestedEarlyLateBalance01.load (std::memory_order_relaxed),
                                                      requestedFreeze.load (std::memory_order_relaxed));
    }

    convolution.prepare (spec);

    preDelayLine.prepare (spec);

    // See docs/architecture.md ("DryWetMixer gotcha"): juce::dsp::Chorus
    // owns its own internal DryWetMixer, primed the same way - its
    // prepare() calls update() (which sets the mixer's *target* wet
    // proportion from whatever setMix() last configured) before its own
    // reset(). Configuring the chorus's parameters here, before calling
    // prepare(), ensures the very first process() call after (re)prepare
    // already reflects lastModulationAmount01 instead of the class's own
    // built-in defaults (rate 1 Hz / depth 0.25 / mix 0.5).
    modulationChorus.setRate (modulationRateHz);
    modulationChorus.setCentreDelay (modulationCentreDelayMs);
    modulationChorus.setFeedback (0.0f);
    modulationChorus.setDepth (mapModulationDepth (lastModulationAmount01));
    modulationChorus.setMix (mapModulationMix (lastModulationAmount01));
    modulationChorus.prepare (spec);

    outputGain.setRampDurationSeconds (smoothingTimeSeconds);
    outputGain.prepare (spec);

    dryWetMixer.prepare (spec);

    // juce::dsp::Convolution's default configuration (used here) is
    // zero-latency and uniformly partitioned, so this is normally 0 -
    // queried rather than assumed so the plugin stays correct if a fixed-
    // latency configuration is ever adopted instead. Modulation (chorus)
    // adds no reported latency (see getLatencySamples()'s doc comment).
    latencySamples = convolution.getLatency();
    dryWetMixer.setWetLatency (static_cast<float> (latencySamples));

    // See docs/architecture.md ("DryWetMixer gotcha") for the full
    // explanation: priming the real target here, before reset() runs, means
    // the mixer is already sitting at the correct dry/wet balance for the
    // very first process() call instead of ramping up from "fully wet" over
    // its internal ~50 ms default ramp.
    dryWetMixer.setWetMixProportion (lastMixProportion);

    preDelayMsSmoothed.reset (sampleRate, smoothingTimeSeconds);
    preDelayMsSmoothed.setCurrentAndTargetValue (lastPreDelayMs);
    widthAmountSmoothed.reset (sampleRate, smoothingTimeSeconds);
    widthAmountSmoothed.setCurrentAndTargetValue (lastWidthPercent * 0.01f);

    reset();

    // Prime the delay line's sample count immediately - reset() clears
    // buffer/position state but knows nothing about lastPreDelayMs - so the
    // very first process() call already uses the correct Pre-Delay instead
    // of ramping up from 0.
    const auto preDelaySamples = juce::jlimit (0.0f,
                                                static_cast<float> (preDelayLine.getMaximumDelayInSamples()),
                                                lastPreDelayMs * 0.001f * static_cast<float> (sampleRate));
    preDelayLine.setDelay (preDelaySamples);
}

void ReverbEngine::reset()
{
    preDelayLine.reset();
    convolution.reset();
    modulationChorus.reset();
    outputGain.reset();
    dryWetMixer.reset();
}

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
    // juce::dsp::DryWetMixer smooths this internally (~50 ms ramp), so no
    // extra SmoothedValue is needed on this side - re-applying the target
    // every block is cheap and keeps automation zipper-free.
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
    // juce::dsp::Chorus smooths depth (via its own internal SmoothedValue)
    // and mix (via its own internal DryWetMixer's ~50 ms ramp) itself, so -
    // like Mix above - re-applying the target every block is cheap and
    // keeps automation zipper-free without an extra SmoothedValue here.
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
}

void ReverbEngine::regenerateImpulseResponseIfNeeded()
{
    if (usingUserImpulseResponse)
        return; // A user IR override is active; these parameters don't drive the procedural generator while it is.

    const auto decay = requestedDecaySeconds.load (std::memory_order_relaxed);
    const auto damping = requestedDampingHz.load (std::memory_order_relaxed);
    const auto space = static_cast<ReverbIR::SpaceType> (requestedSpace.load (std::memory_order_relaxed));
    const auto earlyLateBalance = requestedEarlyLateBalance01.load (std::memory_order_relaxed);
    const auto freeze = requestedFreeze.load (std::memory_order_relaxed);

    // Small epsilon avoids reloading on floating-point noise from repeated
    // identical parameter pushes (e.g. a host re-sending the same
    // automation value on every block).
    constexpr float epsilon = 1.0e-3f;

    const auto decayChanged = std::abs (decay - lastGeneratedDecaySeconds) >= epsilon;
    const auto dampingChanged = std::abs (damping - lastGeneratedDampingHz) >= epsilon;
    const auto spaceChanged = space != lastGeneratedSpace;
    const auto balanceChanged = std::abs (earlyLateBalance - lastGeneratedEarlyLateBalance01) >= epsilon;
    const auto freezeChanged = freeze != lastGeneratedFreeze;

    if (! decayChanged && ! dampingChanged && ! spaceChanged && ! balanceChanged && ! freezeChanged)
        return;

    queueProceduralImpulseResponse (decay, damping, space, earlyLateBalance, freeze);
}

bool ReverbEngine::loadUserImpulseResponse (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    // Sanity-check the file is actually readable audio (and not
    // pathologically long) *before* handing it to juce::dsp::Convolution,
    // so a bogus/mis-selected file leaves usingUserImpulseResponse/
    // userImpulseResponseFile - and therefore the currently active IR -
    // completely unchanged rather than silently switching to a file
    // Convolution itself couldn't actually load.
    std::unique_ptr<juce::AudioFormatReader> reader (userIrFormatManager.createReaderFor (file));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const auto durationSeconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    reader.reset(); // release the reader before Convolution opens the file itself

    if (durationSeconds > maxUserImpulseResponseSeconds)
        return false;

    // Hand the (already-validated) file off for process() to actually load
    // on the audio thread (see the class comment's "Threading" section) -
    // juce::File's copy is refcounted/non-allocating, and the
    // File-taking loadImpulseResponse() overload itself only enqueues a
    // background-loader command, so this costs nothing extra beyond the
    // validation already done above.
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse.kind = PendingImpulseResponseKind::userFile;
        pendingImpulseResponse.userFile = file;
    }

    usingUserImpulseResponse = true;
    userImpulseResponseFile = file;
    return true;
}

void ReverbEngine::clearUserImpulseResponse()
{
    if (! usingUserImpulseResponse)
        return;

    usingUserImpulseResponse = false;
    userImpulseResponseFile = juce::File();

    // Reload the procedural IR immediately (rather than waiting for the
    // next regenerateImpulseResponseIfNeeded() tick) so the revert is
    // effective as soon as process() next runs.
    queueProceduralImpulseResponse (requestedDecaySeconds.load (std::memory_order_relaxed),
                                     requestedDampingHz.load (std::memory_order_relaxed),
                                     static_cast<ReverbIR::SpaceType> (requestedSpace.load (std::memory_order_relaxed)),
                                     requestedEarlyLateBalance01.load (std::memory_order_relaxed),
                                     requestedFreeze.load (std::memory_order_relaxed));
}

void ReverbEngine::loadProceduralImpulseResponseSynchronously (float decaySeconds, float dampingHz,
                                                                 ReverbIR::SpaceType space, float earlyLateBalance01, bool freeze)
{
    auto impulseResponse = ReverbIR::generateProceduralImpulseResponse (sampleRate, decaySeconds, dampingHz, numChannels,
                                                                          space, earlyLateBalance01, freeze);

    const auto stereo = numChannels >= 2 ? juce::dsp::Convolution::Stereo::yes
                                          : juce::dsp::Convolution::Stereo::no;

    convolution.loadImpulseResponse (std::move (impulseResponse), sampleRate, stereo,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::yes);

    lastGeneratedDecaySeconds = decaySeconds;
    lastGeneratedDampingHz = dampingHz;
    lastGeneratedSpace = space;
    lastGeneratedEarlyLateBalance01 = earlyLateBalance01;
    lastGeneratedFreeze = freeze;
}

void ReverbEngine::queueProceduralImpulseResponse (float decaySeconds, float dampingHz,
                                                     ReverbIR::SpaceType space, float earlyLateBalance01, bool freeze)
{
    // The non-real-time-safe part (heap allocation, per-sample exp()/random
    // calls) happens here, on the message thread, exactly as before -
    // building `impulseResponse` doesn't touch the audio thread at all.
    auto impulseResponse = ReverbIR::generateProceduralImpulseResponse (sampleRate, decaySeconds, dampingHz, numChannels,
                                                                          space, earlyLateBalance01, freeze);

    // Hand the finished buffer off for process() to actually load on the
    // audio thread (see the class comment's "Threading" section). The
    // critical section is just a move-assignment (no allocation - it
    // transfers the buffer's already-allocated storage) so this lock is
    // held for a negligible, bounded duration.
    {
        const juce::SpinLock::ScopedLockType lock (pendingImpulseResponseLock);
        pendingImpulseResponse.kind = PendingImpulseResponseKind::procedural;
        pendingImpulseResponse.proceduralBuffer = std::move (impulseResponse);
        pendingImpulseResponse.proceduralSampleRate = sampleRate;
    }

    lastGeneratedDecaySeconds = decaySeconds;
    lastGeneratedDampingHz = dampingHz;
    lastGeneratedSpace = space;
    lastGeneratedEarlyLateBalance01 = earlyLateBalance01;
    lastGeneratedFreeze = freeze;
}

void ReverbEngine::applyPendingImpulseResponseIfAny() noexcept
{
    const juce::SpinLock::ScopedTryLockType lock (pendingImpulseResponseLock);

    if (! lock.isLocked() || pendingImpulseResponse.kind == PendingImpulseResponseKind::none)
        return;

    const auto stereo = numChannels >= 2 ? juce::dsp::Convolution::Stereo::yes
                                          : juce::dsp::Convolution::Stereo::no;

    if (pendingImpulseResponse.kind == PendingImpulseResponseKind::procedural)
    {
        // The AudioBuffer-taking overload takes ownership of the buffer
        // (moved out below) rather than allocating - see juce_Convolution.h:
        // "To avoid memory allocation on the audio thread, this function
        // takes ownership of the buffer passed in."
        convolution.loadImpulseResponse (std::move (pendingImpulseResponse.proceduralBuffer),
                                          pendingImpulseResponse.proceduralSampleRate,
                                          stereo,
                                          juce::dsp::Convolution::Trim::no,
                                          juce::dsp::Convolution::Normalise::yes);
    }
    else // userFile
    {
        // juce::File's copy (taken internally by this overload) is
        // refcounted, not allocating; the actual file read/decode happens
        // asynchronously on juce::dsp::Convolution's own background thread,
        // never blocking this call.
        convolution.loadImpulseResponse (pendingImpulseResponse.userFile,
                                          stereo,
                                          juce::dsp::Convolution::Trim::yes,
                                          0,
                                          juce::dsp::Convolution::Normalise::yes);
    }

    pendingImpulseResponse.kind = PendingImpulseResponseKind::none;
}

void ReverbEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    // The only place juce::dsp::Convolution::loadImpulseResponse() is ever
    // called outside prepare() - see the class comment's "Threading"
    // section and applyPendingImpulseResponseIfAny()'s own doc comment.
    applyPendingImpulseResponseIfAny();

    // Capture the pre-processing signal as "dry" before Pre-Delay/
    // convolution/Modulation/Width touch `block`. DryWetMixer internally
    // delays this by getLatencySamples() (set via setWetLatency in
    // prepare()) so it stays time-aligned with the (normally zero-latency)
    // wet path below. Pre-Delay is deliberately *not* part of this
    // compensation - it is an audible effect parameter (the gap before the
    // tail), not something to hide from the dry signal.
    dryWetMixer.pushDrySamples (block);

    const auto preDelaySamples = juce::jlimit (0.0f,
                                                static_cast<float> (preDelayLine.getMaximumDelayInSamples()),
                                                preDelayMsSmoothed.skip (static_cast<int> (numSamples)) * 0.001f * static_cast<float> (sampleRate));
    preDelayLine.setDelay (preDelaySamples);

    juce::dsp::ProcessContextReplacing<float> context (block);

    preDelayLine.process (context);
    convolution.process (context);
    modulationChorus.process (context);

    applyWidth (block);

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
