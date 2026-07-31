// Hardening pass against the bug class found and fixed in basilica-audio/nave
// (Nave PR #28, issue #27): pluginval crashed with "libc++abi: terminating
// due to uncaught exception of type std::__1::bad_function_call" because
// Nave's CabConvolutionEngine had "message-thread only" methods
// (setAlignMode/setGainMode/setIrAMinPhase/setIrBMinPhase, each reloading a
// juce::dsp::Convolution slot) reachable from TWO unsynchronised entry
// points: prepareToPlay() (called by the host on whatever thread it
// chooses - the VST3/AU contract guarantees only that this is not the audio
// thread, NOT that it is JUCE's own MessageManager thread) and
// AsyncUpdater::handleAsyncUpdate() (always the real message thread,
// triggered by audio-thread-delivered automation). Nave fixed this with a
// std::recursive_mutex (messageThreadMutex) taken at the top of every
// message-thread-only method - see that repo's src/dsp/
// CabConvolutionEngine.h/.cpp and tests/CrossThreadReprepareTests.cpp, this
// file's namesake and template.
//
// ==========================================================================
// AUDIT (this task's step 2): every entry point into MorphingConvolution and
// ReverbEngine reachable from more than one non-audio thread.
//
// 1. MorphingConvolution itself: NOT where the bug lives. Its own doc
//    contract (src/dsp/MorphingConvolution.h, class comment) is already
//    correctly partitioned and, unlike Nave's CabConvolutionEngine, is
//    actually upheld by every caller in this codebase:
//      - loadKernelSynchronously()/loadFileSynchronously() ("Message-thread
//        only, and only while processing is suspended") are called from
//        exactly one call site: ReverbEngine::prepare().
//      - postKernel()/postFile() ("Audio-thread only") are called from
//        exactly one call site: ReverbEngine::applyPendingImpulseResponseIfAny(),
//        itself called only from ReverbEngine::process() (the audio thread).
//    Those two groups therefore never race each other directly, PROVIDED
//    the host never calls prepareToPlay() concurrently with processBlock()
//    for the same instance - a assumption every JUCE plugin (including
//    Nave, pre-fix) already relies on and that held even in Nave's crash.
//    So the Convolution-level race Nave hit does not reproduce here
//    verbatim. The actual defect is one level up, in ReverbEngine's OWN
//    cross-thread state - see #2 and #3.
//
// 2. ReverbEngine::prepare() vs. the background "Requiem IR Render" thread
//    (IrRenderThread::run() -> runRenderLoop() -> renderOnce()) - THE REAL
//    DEFECT, structurally identical in shape to Nave's #27/#28: two
//    non-audio threads with unsynchronised access to the same mutable
//    state.
//
//    ReverbEngine::prepare() (src/dsp/ReverbEngine.h line ~71) is
//    documented "Not real-time safe; call only from the message thread" -
//    but its one and only real caller, RequiemAudioProcessor::
//    prepareToPlay(), is called by the host on whatever thread the host
//    chooses (the VST3/AU contract only guarantees "not the audio thread"),
//    exactly the same false "it's the message thread" assumption Nave's
//    CabConvolutionEngine::prepare() doc comment made pre-fix (see Nave PR
//    #28's root-cause writeup). Pre-fix here, prepare() directly, with NO
//    lock:
//      - overwrites ReverbEngine::sampleRate / numChannels / maximumBlockSize
//        (plain double/int members, not atomics);
//      - calls fdnTail.prepare(spec), which reassigns FdnTail::delayLengths
//        (a std::array<int,16>) via primeDelayLengths() and calls
//        lineBuffers[line].assign(...) - a real reallocation - on all 16
//        delay-line vectors;
//      - calls renderOnce(buildRequest()) SYNCHRONOUSLY on its own (host)
//        thread.
//    Meanwhile runRenderLoop() (src/dsp/ReverbEngine.cpp, the render
//    thread's loop) calls renderOnce() OUTSIDE requestLock - only the
//    request-slot bookkeeping (requestedRender/lastRenderedRequest) is
//    lock-protected, not the render call itself. renderOnce() reads
//    ReverbEngine::sampleRate/numChannels (ReverbIR::
//    generateProceduralImpulseResponse(sampleRate, ..., numChannels, ...),
//    IrAnalysis::analyse(..., sampleRate)) and calls
//    fdnTail.getDelayLengths()/getShortestDelaySamples() while fitting the
//    FDN attenuation - the SAME fields prepare() mutates above, with zero
//    synchronisation between the two threads. Textbook data race (unsafe
//    per the C++ memory model regardless of whether a given run happens to
//    observe a torn value), and, chased through to its consequence, a real
//    memory-safety hazard: if the background thread's renderOnce() (started
//    for a pre-reprepare request) finishes AFTER prepare() has already
//    cleared+repopulated pendingImpulseResponse for the NEW spec, it
//    clobbers pendingImpulseResponse with a buffer sized/sampled for the
//    OLD spec. ReverbEngine::process() (audio thread) then hands that
//    stale buffer to MorphingConvolution::postKernel() together with the
//    CURRENT (already-changed) numChannels
//    (applyPendingImpulseResponseIfAny(), ReverbEngine.cpp) - i.e. a buffer
//    whose actual channel count was baked in for the old numChannels can be
//    told it is for a different numChannels, straight into
//    juce::dsp::Convolution::loadImpulseResponse() on an engine that was
//    just re-prepared for a third (also different) sample rate.
//
// 3. usingUserImpulseResponse / userImpulseResponseFile - plain bool/
//    juce::File members, not atomics, not lock-protected anywhere:
//      - WRITTEN by ReverbEngine::loadUserImpulseResponse()/
//        clearUserImpulseResponse() ("Message-thread only" per their doc
//        comments). Their two real callers are
//        RequiemAudioProcessor::loadUserImpulseResponseFile() (message
//        thread in practice - a GUI FileChooser callback) AND
//        RequiemAudioProcessor::setStateInformation() (PluginProcessor.cpp),
//        which calls engine.loadUserImpulseResponse()/
//        clearUserImpulseResponse() directly - and setStateInformation(),
//        like prepareToPlay(), is called by the host on a
//        host-chosen thread with NO guarantee it is the message thread.
//        That is two genuinely different non-audio threads writing the
//        same unsynchronised fields, the closest structural match in this
//        codebase to Nave's exact "prepareToPlay-thread vs.
//        AsyncUpdater-message-thread" race.
//      - READ by ReverbEngine::prepare() (the "if (usingUserImpulseResponse
//        && ...)" branch, ReverbEngine.cpp) and by buildRequest() (called
//        from prepare(), regenerateImpulseResponseIfNeeded(), and the two
//        loadUserImpulseResponse()/clearUserImpulseResponse() writers
//        themselves) - juce::File internally ref-counts a shared String
//        payload, so a read racing a write here is not merely "might see a
//        stale value" but a potential torn/concurrent mutation of that
//        ref-counted payload.
//
// 4. std::function/callback-before-assignment audit: ReverbEngine and
//    MorphingConvolution hold no std::function members at all (juce::Timer
//    and juce::Thread are virtual-dispatch, not std::function-based, so
//    there is no default-constructed-empty-std::function window
//    equivalent to Nave's BackgroundMessageQueue::push() failure mode).
//    juce::dsp::Convolution's own ConvolutionMessageQueue is shared
//    correctly (MorphingConvolution's constructor comment) between exactly
//    the two engines that are, per #1 above, never loaded from more than
//    one thread at a time - so that queue's own single-thread-safe push()
//    contract is upheld here, unlike pre-fix Nave.
//
// THE FIX (src/dsp/ReverbEngine.{h,cpp}): a std::recursive_mutex
// (reconfigureMutex, mirroring Nave's messageThreadMutex naming/pattern)
// taken by prepare() (its entire body), by loadUserImpulseResponse()/
// clearUserImpulseResponse(), by buildRequest() (so every reader of
// usingUserImpulseResponse/userImpulseResponseFile is covered even when
// called from regenerateImpulseResponseIfNeeded(), which does not itself
// need the lock for anything else), and around the renderOnce() call inside
// runRenderLoop() (NOT around the whole render loop - only the render call
// itself needs to be mutually exclusive with prepare(), and the loop's own
// requestLock-protected bookkeeping stays as-is). Recursive because
// prepare() calls renderOnce()+buildRequest() while already holding it,
// same reason Nave's mutex is recursive. Never taken by process(),
// applyPendingImpulseResponseIfAny(), or applyPendingHybridSetupIfAny() (the
// audio-thread path), which keep their existing SpinLock::
// ScopedTryLockType/lock-free design - no lock or allocation is added to
// the audio thread.
//
// ==========================================================================
// THIS TEST reproduces the concurrency directly, ReverbEngine-only (no
// AudioProcessor/APVTS needed - the race lives entirely inside ReverbEngine,
// see #2/#3 above), with three real std::threads standing in for: the
// host's prepareToPlay()/setStateInformation()-calling thread ("host"), the
// message-thread's ~20 Hz regenerateImpulseResponseIfNeeded() timer plus
// GUI-driven user-IR load/clear ("message"), and the audio thread's
// process() ("audio") - all running concurrently and genuinely, exactly the
// three-way concurrency a real host (or pluginval) may present.
//
// Being a genuine data race, this is a best-effort empirical reproduction,
// not a guaranteed one on every run (see the PR body for the red/green
// verification evidence gathered during development). The actual safety
// guarantee is reconfigureMutex, which makes the race structurally
// impossible; this test exists as a trip-wire against that mutex being
// removed or bypassed in future - exactly Nave's CrossThreadReprepareTests.cpp
// rationale, applied to this engine's own actual defect.
#include "dsp/ReverbEngine.h"
#include "TestHelpers.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    // Writes a short, valid impulse-response WAV to a temp file so the
    // user-IR load/clear path (loadUserImpulseResponse()/postFile()) gets
    // real file I/O and real decode work, not just the trivial default-IR
    // path. Mirrors tests/StateTests.cpp's writeTestImpulseResponseFile().
    juce::File writeCrossThreadTestIrFile (juce::Random& random)
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("requiem_crossthread_ir_" + juce::String (random.nextInt64()) + ".wav");

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());

        if (stream == nullptr)
            return {};

        auto writer = wavFormat.createWriterFor (stream,
            juce::AudioFormatWriterOptions()
                .withSampleRate (48000.0)
                .withNumChannels (2)
                .withBitsPerSample (16));

        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> irBuffer (2, 512);
        irBuffer.clear();

        for (int i = 0; i < irBuffer.getNumSamples(); ++i)
        {
            const auto value = static_cast<float> (std::sin (i * 0.03) * std::exp (-i / 200.0));
            irBuffer.setSample (0, i, value);
            irBuffer.setSample (1, i, value * 0.8f);
        }

        writer->writeFromAudioSampleBuffer (irBuffer, 0, irBuffer.getNumSamples());
        writer.reset(); // flush/close before any reader (including Convolution) opens it

        return file;
    }
}

//==============================================================================
TEST_CASE ("Concurrent prepare() (simulated host thread) and message-thread automation-driven "
           "reconfigure survive a 44.1/96/192k reprepare sweep with mono/stereo and small/large blocks",
           "[dsp][engine][threading][v030]")
{
    // Thread shape deliberately mirrors basilica-audio/nave's
    // CrossThreadReprepareTests.cpp: prepare() and process() are called
    // from the SAME thread, in the same order a real host uses them
    // (prepare, then process a run of blocks against that spec) - so they
    // are never concurrent WITH EACH OTHER. That is not a simplification of
    // the race, it is the actual contract every JUCE plugin (and this
    // task's own bug-class description - "the host thread
    // (prepareToPlay/releaseResources/setStateInformation) and the message
    // thread") relies on: a host does not call processBlock() while
    // prepareToPlay() is still running for the same instance. (An earlier
    // version of this test used a fully independent audio thread here and
    // reliably crashed - but that was ReverbEngine::prepare() -> fdnTail.
    // prepare() reallocating FdnTail's delay-line juce::AudioBuffer-backed
    // vectors concurrently with FdnTail::process() reading them on a
    // genuinely unrelated thread, i.e. the fundamental prepareToPlay()/
    // processBlock() mutual-exclusion contract every real host already
    // upholds - not the host-thread-vs-message-thread bug class this task
    // targets, and not reachable by any real host or by pluginval.) What
    // legitimately races prepare() here, on separate threads, are exactly
    // the two the audit identified: the background "Requiem IR Render"
    // thread's renderOnce() call, and the message thread's
    // regenerateImpulseResponseIfNeeded()/loadUserImpulseResponse()/
    // clearUserImpulseResponse().
    ReverbEngine engine;
    engine.setDecaySeconds (0.2f); // short: keeps each render fast, maximises reprepare/render frequency
    engine.setDampingHz (6000.0f);
    engine.setMixProportion (1.0f);

    // Deterministic seed, per the task's "deterministic seeds" requirement -
    // every run of this test walks the exact same sequence of sample
    // rates/block sizes/channel counts/parameter values, so a reproduction
    // (or its absence) is reproducible across runs, not just within one.
    juce::Random random (0x5EED0001);

    {
        juce::dsp::ProcessSpec initialSpec { 48000.0, 512, 2 };
        engine.prepare (initialSpec);
    }

    std::atomic<bool> stop { false };
    std::atomic<bool> sawNonFiniteOutput { false };
    std::atomic<int> hostIterationsCompleted { 0 };

    constexpr int hostIterations = 24; // x 3 rates x 2 block sizes x 2 channel counts below
    const std::array<double, 3> sampleRates { 44100.0, 96000.0, 192000.0 };
    const std::array<int, 2> blockSizes { 64, 1024 };
    const std::array<int, 2> channelCounts { 1, 2 };

    // Simulates the host's own prepareToPlay()-calling thread - per the
    // VST3/AU contract not guaranteed to be JUCE's message thread (see the
    // audit above) - repeatedly re-preparing across the full sample-rate/
    // block-size/channel-count matrix, processing a short run of blocks
    // against each freshly-prepared spec (the sequencing a real host
    // upholds - see the class-level note above), while the "message" thread
    // below concurrently drives regenerateImpulseResponseIfNeeded() and
    // user-IR swaps.
    std::thread hostThread ([&]
    {
        for (int iteration = 0; iteration < hostIterations; ++iteration)
        {
            const auto sr = sampleRates[static_cast<size_t> (iteration) % sampleRates.size()];
            const auto blockSize = blockSizes[static_cast<size_t> (iteration) % blockSizes.size()];
            const auto channels = channelCounts[static_cast<size_t> (iteration / 2) % channelCounts.size()];

            juce::dsp::ProcessSpec spec { sr, static_cast<juce::uint32> (blockSize), static_cast<juce::uint32> (channels) };
            engine.prepare (spec);

            juce::AudioBuffer<float> buffer (static_cast<int> (channels), blockSize);
            juce::int64 sampleCounter = 0;

            for (int block = 0; block < 4; ++block)
            {
                TestHelpers::fillWithSine (buffer, sr, 220.0, 0.5f, sampleCounter);
                sampleCounter += blockSize;

                juce::dsp::AudioBlock<float> audioBlock (buffer);
                engine.process (audioBlock);

                if (! TestHelpers::allSamplesFinite (buffer))
                    sawNonFiniteOutput.store (true, std::memory_order_relaxed);
            }

            hostIterationsCompleted.store (iteration + 1, std::memory_order_relaxed);
            std::this_thread::yield();
        }

        stop.store (true, std::memory_order_relaxed);
    });

    // Simulates the message thread: the ~20 Hz regenerateImpulseResponseIfNeeded()
    // timer (automation-driven reconfiguration: Decay/Damping/Space/Size/
    // BassDecay/EngineMode/Bloom all change every iteration, so a new kernel
    // and/or a new FDN fit is generated on essentially every call) plus
    // occasional user-IR load/clear, standing in for a GUI FileChooser
    // callback landing on the message thread mid-reprepare.
    std::thread messageThread ([&]
    {
        int i = 0;

        while (! stop.load (std::memory_order_relaxed))
        {
            const auto decay = 0.1f + 0.3f * random.nextFloat();
            const auto damping = 2000.0f + 8000.0f * random.nextFloat();
            const auto space = static_cast<ReverbIR::SpaceType> (i % 3);
            const auto mode = static_cast<ReverbEngine::EngineMode> (i % 3);

            engine.setDecaySeconds (decay);
            engine.setDampingHz (damping);
            engine.setSpaceType (space);
            engine.setSize (random.nextFloat());
            engine.setBassDecayMultiplier (0.5f + random.nextFloat());
            engine.setEngineMode (mode);
            engine.setBloomAmount (random.nextFloat());

            engine.regenerateImpulseResponseIfNeeded();

            if (i % 17 == 0)
            {
                // Left in the OS temp directory rather than deleted here:
                // deleting while ReverbEngine's background render thread
                // might still be mid-decode of this exact file would just
                // be testing juce::AudioFormatReader's own file-lifetime
                // handling, not the cross-thread engine race this test
                // targets.
                auto irFile = writeCrossThreadTestIrFile (random);

                if (irFile.existsAsFile())
                    engine.loadUserImpulseResponse (irFile);
            }
            else if (i % 23 == 0)
            {
                engine.clearUserImpulseResponse();
            }

            ++i;
            std::this_thread::yield();
        }

        engine.clearUserImpulseResponse();
    });

    hostThread.join();
    messageThread.join();

    CHECK (hostIterationsCompleted.load() == hostIterations);
    CHECK_FALSE (sawNonFiniteOutput.load());
}

//==============================================================================
TEST_CASE ("Concurrent loadUserImpulseResponse()/clearUserImpulseResponse() (simulated "
           "setStateInformation-from-host-thread vs. GUI-from-message-thread) survive racing "
           "against prepare() and process()",
           "[dsp][engine][threading][v030]")
{
    // Targets audit finding #3 specifically: usingUserImpulseResponse/
    // userImpulseResponseFile are plain, unsynchronised members written from
    // two genuinely different non-audio-thread call sites in production
    // (RequiemAudioProcessor::setStateInformation(), host-thread-called; and
    // a GUI FileChooser callback, message-thread-called). This test drives
    // both call sites concurrently, plus a third thread re-preparing (which
    // also reads those two fields) and processes a short run of audio right
    // after each prepare() - on that SAME thread, sequenced, never
    // concurrent with its own prepare() calls (see the class-level note in
    // the "reprepare sweep" TEST_CASE above for why: that is the actual
    // prepareToPlay()/processBlock() contract every host upholds, not a
    // simplification of the race under test).
    ReverbEngine engine;
    engine.setDecaySeconds (0.15f);
    engine.setDampingHz (5000.0f);
    engine.setMixProportion (1.0f);

    juce::Random randomA (0x5EED0002);
    juce::Random randomB (0x5EED0003);

    {
        juce::dsp::ProcessSpec initialSpec { 44100.0, 256, 2 };
        engine.prepare (initialSpec);
    }

    std::atomic<bool> sawNonFiniteOutput { false };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (12);

    // "setStateInformation()"-style caller: host-controlled thread, per the
    // audit not guaranteed to be the message thread.
    std::thread stateInfoThread ([&]
    {
        int i = 0;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (i % 2 == 0)
            {
                auto irFile = writeCrossThreadTestIrFile (randomA);

                if (irFile.existsAsFile())
                    engine.loadUserImpulseResponse (irFile);
            }
            else
            {
                engine.clearUserImpulseResponse();
            }

            ++i;
            std::this_thread::yield();
        }
    });

    // GUI FileChooser-style caller: the real message thread in production.
    std::thread guiThread ([&]
    {
        int i = 0;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (i % 3 == 0)
            {
                auto irFile = writeCrossThreadTestIrFile (randomB);

                if (irFile.existsAsFile())
                    engine.loadUserImpulseResponse (irFile);
            }
            else
            {
                engine.clearUserImpulseResponse();
            }

            engine.regenerateImpulseResponseIfNeeded();

            ++i;
            std::this_thread::yield();
        }
    });

    // Concurrent reprepare, sweeping rates - also reads
    // usingUserImpulseResponse/userImpulseResponseFile (see audit #3) - with
    // process() called right after each prepare() on this same thread (the
    // real host's own sequencing; see this TEST_CASE's opening comment).
    std::thread prepareThread ([&]
    {
        const std::array<double, 3> sampleRates { 44100.0, 96000.0, 192000.0 };
        constexpr int blockSize = 128;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::int64 sampleCounter = 0;
        int i = 0;

        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto sr = sampleRates[static_cast<size_t> (i) % sampleRates.size()];
            juce::dsp::ProcessSpec spec { sr, static_cast<juce::uint32> (blockSize), 2 };
            engine.prepare (spec);

            for (int block = 0; block < 3; ++block)
            {
                TestHelpers::fillWithSine (buffer, sr, 330.0, 0.5f, sampleCounter);
                sampleCounter += blockSize;

                juce::dsp::AudioBlock<float> audioBlock (buffer);
                engine.process (audioBlock);

                if (! TestHelpers::allSamplesFinite (buffer))
                    sawNonFiniteOutput.store (true, std::memory_order_relaxed);
            }

            ++i;
            std::this_thread::yield();
        }
    });

    stateInfoThread.join();
    guiThread.join();
    prepareThread.join();

    engine.clearUserImpulseResponse();

    CHECK_FALSE (sawNonFiniteOutput.load());
}
