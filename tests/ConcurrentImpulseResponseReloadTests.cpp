// Regression coverage for issue #13 (data race: juce::dsp::Convolution::
// loadImpulseResponse() called from the message thread races with
// process() on the audio thread) and issue #14 (no test exercised the real
// concurrent Timer-vs-audio-thread IR reload path).
//
// Pre-fix, ReverbEngine::regenerateImpulseResponseIfNeeded() called
// convolution.loadImpulseResponse() directly, from whichever thread called
// it. Every other test in this suite (see EngineTests.cpp) only ever calls
// regenerateImpulseResponseIfNeeded() and process() sequentially on a
// single thread, so the actual bug - two threads mutating
// juce::dsp::Convolution's internal (non-atomic) pending-command state at
// the same time - was never exercised anywhere in the suite (see issue #14
// for the audit that found this gap).
//
// This test runs two real std::threads - one repeatedly calling
// ReverbEngine::regenerateImpulseResponseIfNeeded() (simulating
// RequiemAudioProcessor's message-thread juce::Timer), the other repeatedly
// calling ReverbEngine::process() (simulating the audio thread) - genuinely
// concurrently, for a sustained burst of reload/process calls, hammering
// both paths far harder than the ~20 Hz production timer ever would.
//
// Post-fix, ReverbEngine::regenerateImpulseResponseIfNeeded() only
// generates a buffer and hands it to a SpinLock-guarded slot;
// convolution.loadImpulseResponse() is only ever called from process()
// (the audio thread) - see ReverbEngine::applyPendingImpulseResponseIfAny().
// That structural change is what actually fixes issue #13; this test is a
// best-effort empirical stress reproduction of the pre-fix race (races are
// inherently timing-dependent, not deterministic) rather than a substitute
// for it - see the PR description for how this was verified against the
// pre-fix code locally.
#include "dsp/ReverbEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

TEST_CASE ("Concurrent regenerateImpulseResponseIfNeeded() (simulated message thread) vs. process() "
           "(simulated audio thread) does not crash or produce non-finite output",
           "[dsp][engine][concurrency]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64; // small: maximises process() call frequency for the run's duration.
    constexpr int numMessageIterations = 3000;

    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setDecaySeconds (0.2f);
    engine.setDampingHz (6000.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
    spec.numChannels = 2;
    engine.prepare (spec);

    std::atomic<bool> stop { false };
    std::atomic<bool> sawNonFinite { false };

    // Simulated audio thread: processes small blocks back-to-back for as
    // long as the message thread below is still hammering reloads, plus a
    // hard wall-clock cap as a safety net against any pathological hang.
    std::thread audioThread ([&]
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::int64 sampleCounter = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (15);

        while (! stop.load (std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
        {
            TestHelpers::fillWithSine (buffer, sampleRate, 220.0, 0.5f, sampleCounter);
            sampleCounter += blockSize;

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            if (! TestHelpers::allSamplesFinite (buffer))
                sawNonFinite.store (true, std::memory_order_relaxed);
        }
    });

    // Simulated message thread: alternates Decay/Damping every iteration so
    // a new IR is generated (and handed off) on essentially every call -
    // maximum reload pressure against the concurrently running audio
    // thread, exactly the call pattern that raced pre-fix.
    std::thread messageThread ([&]
    {
        for (int i = 0; i < numMessageIterations; ++i)
        {
            const auto decay = (i % 2 == 0) ? 0.15f : 0.4f;
            const auto damping = (i % 2 == 0) ? 3000.0f : 9000.0f;

            engine.setDecaySeconds (decay);
            engine.setDampingHz (damping);
            engine.regenerateImpulseResponseIfNeeded();
        }

        stop.store (true, std::memory_order_relaxed);
    });

    messageThread.join();
    audioThread.join();

    CHECK_FALSE (sawNonFinite.load());
}
