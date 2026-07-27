#include "dsp/ReverbEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <utility>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 1 << 17; // ~2.7 s at 48 kHz: large single
                                            // block, long enough to contain
                                            // a full 1-2 s IR tail plus
                                            // Pre-Delay, and keeps the
                                            // tests below simple by avoiding
                                            // multi-block bookkeeping.
    constexpr double testFrequencyHz = 500.0;

    juce::dsp::ProcessSpec makeTestSpec (int numChannels)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (testBlockSize);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }
}

TEST_CASE ("Engine null test: 0% mix nulls against the input once shifted by latency", "[dsp][engine][null]")
{
    ReverbEngine engine;

    // Parameters other than Mix/Output are deliberately set to non-neutral
    // values: a true null test has to prove the *entire* wet chain is
    // bypassed, not just that it happens to be quiet at default settings.
    // Output is left at 0 dB (neutral) because it is a separate, documented
    // post-mix trim stage (see docs/architecture.md) - deliberately *not*
    // part of the Mix/dry-bypass contract this test is checking, so a
    // non-zero value here would only scale the whole result and tell us
    // nothing extra about whether the wet chain is truly bypassed.
    engine.setMixProportion (0.0f);
    engine.setDecaySeconds (3.0f);
    engine.setDampingHz (2000.0f);
    engine.setPreDelayMs (80.0f);
    engine.setWidthPercent (150.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    const auto latency = engine.getLatencySamples();
    REQUIRE (latency >= 0);
    REQUIRE (latency < testBlockSize / 2);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    const auto overlapLength = testBlockSize - latency;
    REQUIRE (overlapLength > testBlockSize / 2);

    // < -90 dBFS residual, in linear amplitude.
    constexpr float tolerance = 3.1623e-5f; // 10^(-90/20)

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < overlapLength; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[latency + i] - refData[i]));

        CHECK (maxResidual < tolerance);
    }
}

TEST_CASE ("Pre-Delay delays the onset of the wet signal", "[dsp][engine]")
{
    constexpr float preDelayMs = 80.0f;
    const auto preDelaySamples = static_cast<int> (std::round (preDelayMs * 0.001 * testSampleRate));

    ReverbEngine engine;
    engine.setMixProportion (1.0f); // fully wet: isolate the wet path's timing
    engine.setDecaySeconds (1.0f);
    engine.setDampingHz (20000.0f); // as bright/unfiltered as the range allows
    engine.setPreDelayMs (preDelayMs);
    engine.setWidthPercent (100.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // A unit impulse at sample 0, silence afterwards: the convolution's
    // response to this is, by definition, the impulse response itself
    // (delayed by Pre-Delay), so its first non-negligible sample marks the
    // wet tail's onset.
    juce::AudioBuffer<float> impulse (2, testBlockSize);
    impulse.clear();
    impulse.setSample (0, 0, 1.0f);
    impulse.setSample (1, 0, 1.0f);

    juce::dsp::AudioBlock<float> block (impulse);
    engine.process (block);

    constexpr float onsetThreshold = 1.0e-4f;
    const auto onsetSample = TestHelpers::firstSampleAboveThreshold (impulse, onsetThreshold);

    REQUIRE (onsetSample >= 0); // the tail must actually produce audible output somewhere

    // A small tolerance accounts for the convolution engine's own internal
    // block/partition alignment and interpolation - the point of this test
    // is "Pre-Delay measurably delays onset by roughly the requested
    // amount", not "onset lands on an exact sample".
    constexpr int toleranceSamples = 16;
    CHECK (onsetSample >= preDelaySamples - toleranceSamples);
    CHECK (onsetSample <= preDelaySamples + toleranceSamples);
}

TEST_CASE ("Zero Pre-Delay produces a near-immediate wet onset", "[dsp][engine]")
{
    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setDecaySeconds (1.0f);
    engine.setDampingHz (20000.0f);
    engine.setPreDelayMs (0.0f);
    engine.setWidthPercent (100.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> impulse (2, testBlockSize);
    impulse.clear();
    impulse.setSample (0, 0, 1.0f);
    impulse.setSample (1, 0, 1.0f);

    juce::dsp::AudioBlock<float> block (impulse);
    engine.process (block);

    constexpr float onsetThreshold = 1.0e-4f;
    const auto onsetSample = TestHelpers::firstSampleAboveThreshold (impulse, onsetThreshold);

    REQUIRE (onsetSample >= 0);
    CHECK (onsetSample < 16); // essentially immediate, well inside the convolution engine's own reported latency margin
}

TEST_CASE ("reset() clears delay-line/convolution/gain state without crashing", "[dsp][engine]")
{
    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setDecaySeconds (1.5f);
    engine.setDampingHz (8000.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.6f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    CHECK_NOTHROW (engine.reset());
    CHECK (TestHelpers::allSamplesFinite (buffer));

    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.6f);
    CHECK_NOTHROW (engine.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("regenerateImpulseResponseIfNeeded() is a no-op unless Decay/Damping actually changed", "[dsp][engine]")
{
    ReverbEngine engine;
    engine.setDecaySeconds (2.0f);
    engine.setDampingHz (8000.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec); // generates the initial IR

    // No Decay/Damping change since prepare(): this must not crash and must
    // leave the engine in a processable state.
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);
    juce::dsp::AudioBlock<float> block (buffer);
    CHECK_NOTHROW (engine.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));

    // Changing Decay and regenerating must also not crash.
    engine.setDecaySeconds (4.0f);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());

    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);
    CHECK_NOTHROW (engine.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("regenerateImpulseResponseIfNeeded() also reacts to Space/Early-Late-Balance/Freeze changes", "[dsp][engine]")
{
    ReverbEngine engine;
    engine.setDecaySeconds (1.5f);
    engine.setDampingHz (8000.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::dsp::AudioBlock<float> block (buffer);

    auto processAndCheckFinite = [&]
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);
        CHECK_NOTHROW (engine.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    };

    engine.setSpaceType (ReverbIR::SpaceType::cathedral);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();

    engine.setEarlyLateBalance (0.2f);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();

    engine.setFreeze (true);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();

    engine.setFreeze (false);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();
}

TEST_CASE ("regenerateImpulseResponseIfNeeded() also reacts to Size/Bass Decay changes (v0.2.0)", "[dsp][engine][v2]")
{
    ReverbEngine engine;
    engine.setDecaySeconds (1.5f);
    engine.setDampingHz (8000.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::dsp::AudioBlock<float> block (buffer);

    auto processAndCheckFinite = [&]
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);
        CHECK_NOTHROW (engine.process (block));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    };

    engine.setSize (1.0f);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();

    engine.setBassDecayMultiplier (ReverbIR::maxBassDecayMultiplier);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();

    engine.setSize (0.0f);
    engine.setBassDecayMultiplier (ReverbIR::minBassDecayMultiplier);
    CHECK_NOTHROW (engine.regenerateImpulseResponseIfNeeded());
    processAndCheckFinite();
}

TEST_CASE ("Freeze sustains the wet tail's energy well past the non-frozen tail's decay", "[dsp][engine]")
{
    // decaySeconds also sizes the generated IR (impulse-response length ==
    // decaySeconds worth of samples - see ImpulseResponseGenerator.h), so
    // both the frozen and non-frozen cases still have live convolution
    // kernel data at the measurement point below (well before the kernel
    // itself runs out): this test measures the *shape* of the envelope
    // within that kernel, not the (unrelated) point where the convolution
    // kernel itself ends.
    constexpr float decaySeconds = 1.0f;
    // 18 * 2048 / 48000 ~= 0.77 s: past the RT60-style -45 dB point for the
    // non-frozen envelope, but comfortably inside the 1 s (48000-sample)
    // convolution kernel for both cases.
    constexpr int numBlocks = 18;
    constexpr int blockSize = 2048;

    auto runAndMeasureLastBlockRms = [] (bool freeze)
    {
        ReverbEngine engine;
        engine.setMixProportion (1.0f); // fully wet
        engine.setDecaySeconds (decaySeconds);
        engine.setDampingHz (8000.0f);
        engine.setPreDelayMs (0.0f);
        engine.setWidthPercent (100.0f);
        engine.setOutputDb (0.0f);
        engine.setFreeze (freeze);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        engine.prepare (spec);
        engine.regenerateImpulseResponseIfNeeded();

        juce::AudioBuffer<float> buffer (2, blockSize);
        double lastBlockRms = 0.0;

        for (int b = 0; b < numBlocks; ++b)
        {
            buffer.clear();

            if (b == 0)
            {
                buffer.setSample (0, 0, 1.0f);
                buffer.setSample (1, 0, 1.0f);
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            if (b == numBlocks - 1)
                lastBlockRms = TestHelpers::rms (buffer);
        }

        return lastBlockRms;
    };

    const auto normalTailRms = runAndMeasureLastBlockRms (false);
    const auto frozenTailRms = runAndMeasureLastBlockRms (true);

    REQUIRE (std::isfinite (normalTailRms));
    REQUIRE (std::isfinite (frozenTailRms));
    REQUIRE (frozenTailRms > 0.0);

    // At this point in the tail, the non-frozen (RT60-decaying) envelope
    // has dropped well below the frozen (flat-envelope) one.
    CHECK (frozenTailRms > normalTailRms * 10.0);
}

TEST_CASE ("Modulation depth measurably changes the wet tail without affecting latency or introducing NaN/Inf", "[dsp][engine]")
{
    auto runAndCapture = [] (float modulationAmount01)
    {
        ReverbEngine engine;
        engine.setMixProportion (1.0f);
        engine.setDecaySeconds (1.0f);
        engine.setDampingHz (8000.0f);
        engine.setPreDelayMs (0.0f);
        engine.setWidthPercent (100.0f);
        engine.setOutputDb (0.0f);
        engine.setModulationAmount (modulationAmount01);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        const auto latency = engine.getLatencySamples();

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        return std::make_pair (buffer, latency);
    };

    const auto [dryModOutput, dryModLatency] = runAndCapture (0.0f);
    const auto [wetModOutput, wetModLatency] = runAndCapture (1.0f);

    CHECK (TestHelpers::allSamplesFinite (dryModOutput));
    CHECK (TestHelpers::allSamplesFinite (wetModOutput));

    // Modulation is a wet-only, non-latency-adding stage (see
    // docs/architecture.md): it must never change reported latency.
    CHECK (dryModLatency == wetModLatency);

    // Full-depth Modulation must audibly differ from no Modulation.
    double maxAbsoluteDifference = 0.0;

    for (int channel = 0; channel < dryModOutput.getNumChannels(); ++channel)
    {
        const auto* a = dryModOutput.getReadPointer (channel);
        const auto* b = wetModOutput.getReadPointer (channel);

        for (int i = 0; i < dryModOutput.getNumSamples(); ++i)
            maxAbsoluteDifference = std::max (maxAbsoluteDifference, static_cast<double> (std::abs (a[i] - b[i])));
    }

    CHECK (maxAbsoluteDifference > 1.0e-4);
}

//==============================================================================
// v0.3.0 brief test 6.12: real-time safety.
//
// The allocation guard replaces the global operator new/delete pair and counts
// every allocation made while it is armed, on the calling thread only. All the
// replaceable forms are provided and all of them route through malloc/free, so
// there is no possibility of an allocation from one form being freed by
// another.
namespace RealtimeGuard
{
    thread_local bool armed = false;
    thread_local int allocations = 0;

    struct ScopedGuard
    {
        ScopedGuard()  { allocations = 0; armed = true; }
        ~ScopedGuard() { armed = false; }

        int count() const noexcept { return allocations; }
    };
}

void* operator new (std::size_t size)
{
    if (RealtimeGuard::armed)
        ++RealtimeGuard::allocations;

    if (size == 0)
        size = 1;

    if (auto* pointer = std::malloc (size))
        return pointer;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)                              { return ::operator new (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept { if (RealtimeGuard::armed) ++RealtimeGuard::allocations; return std::malloc (size == 0 ? 1 : size); }
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept { if (RealtimeGuard::armed) ++RealtimeGuard::allocations; return std::malloc (size == 0 ? 1 : size); }

void operator delete (void* pointer) noexcept                        { std::free (pointer); }
void operator delete[] (void* pointer) noexcept                      { std::free (pointer); }
void operator delete (void* pointer, std::size_t) noexcept           { std::free (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept         { std::free (pointer); }
void operator delete (void* pointer, const std::nothrow_t&) noexcept  { std::free (pointer); }
void operator delete[] (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }

namespace
{
    ReverbEngine::EngineMode allModes[] = {
        ReverbEngine::EngineMode::classicConvolution,
        ReverbEngine::EngineMode::hybridTail,
        ReverbEngine::EngineMode::tailBloom,
    };

    const char* modeName (ReverbEngine::EngineMode mode)
    {
        switch (mode)
        {
            case ReverbEngine::EngineMode::classicConvolution: return "Classic Convolution";
            case ReverbEngine::EngineMode::hybridTail:         return "Hybrid Tail";
            case ReverbEngine::EngineMode::tailBloom:          return "Tail Bloom";
        }

        return "?";
    }
}

TEST_CASE ("6.12 The allocation guard itself works", "[dsp][engine][realtime]")
{
    // A guard that never fires would make every assertion below vacuous.
    //
    // The allocation has to be one the optimiser cannot remove. A plain
    // `new float[64]` immediately followed by `delete[]` is *not*: [expr.new]
    // explicitly permits an implementation to omit the allocation of a
    // new-expression whose storage is never observably used, and both Clang
    // and MSVC do exactly that at Release optimisation levels - which made
    // this self-test pass in Debug and fail in Release. So the storage is
    // obtained through a direct call to the replaced `::operator new` (a plain
    // function call, not a new-expression, so the elision permission does not
    // apply) and is then written through a volatile pointer, making the
    // allocated memory observably used and the call impossible to elide.
    {
        const RealtimeGuard::ScopedGuard guard;
        auto* deliberate = static_cast<float*> (::operator new (64 * sizeof (float)));
        *static_cast<volatile float*> (deliberate) = 1.0f;
        ::operator delete (deliberate);
        REQUIRE (guard.count() > 0);
    }

    {
        const RealtimeGuard::ScopedGuard guard;
        volatile auto sum = 0.0f;

        for (int i = 0; i < 1000; ++i)
            sum = sum + static_cast<float> (i);

        REQUIRE (guard.count() == 0);
    }
}

TEST_CASE ("6.12 process() allocates nothing in any engine mode", "[dsp][engine][realtime]")
{
    constexpr int blockSize = 256;

    for (auto mode : allModes)
    {
        ReverbEngine engine;
        engine.setMixProportion (0.5f);
        engine.setDecaySeconds (3.0f);
        engine.setEngineMode (mode);
        engine.setTailModMode (FdnTail::ModulationMode::matrix);
        engine.setTailModDepth (0.5f);
        engine.setBloomAmount (0.4f);
        engine.setLowCutHz (120.0f);
        engine.setHighCutHz (9000.0f);
        engine.setDuckAmountPercent (50.0f);

        juce::dsp::ProcessSpec spec { testSampleRate, static_cast<juce::uint32> (blockSize), 2 };
        engine.prepare (spec);
        engine.regenerateImpulseResponseIfNeeded();
        REQUIRE (engine.waitForPendingRender());

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::Random random (61);

        // A few unguarded blocks first: the very first call can legitimately
        // touch lazily-initialised JUCE internals.
        for (int block = 0; block < 8; ++block)
        {
            buffer.clear();
            juce::dsp::AudioBlock<float> warmUp (buffer);
            engine.process (warmUp);
        }

        int allocations = 0;

        {
            const RealtimeGuard::ScopedGuard guard;

            for (int block = 0; block < 200; ++block)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto sample = random.nextFloat() * 2.0f - 1.0f;
                    buffer.setSample (0, i, sample);
                    buffer.setSample (1, i, sample);
                }

                // Freeze toggles and engine-mode switches, mid-stream, on the
                // audio thread - exactly what the brief asks the guard to cover.
                if (block % 37 == 0)
                    engine.setFreeze (block % 74 == 0);

                if (block % 53 == 0)
                    engine.setEngineMode (allModes[(block / 53) % 3]);

                juce::dsp::AudioBlock<float> audioBlock (buffer);
                engine.process (audioBlock);
            }

            allocations = guard.count();
        }

        INFO ("engine mode: " << modeName (mode));
        CHECK (allocations == 0);
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("6.12 Silence does not stall the engine on denormals", "[dsp][engine][realtime]")
{
    // Feedback loops that are allowed to fall into denormal arithmetic can run
    // an order of magnitude slower on silence than on signal, which shows up as
    // a plugin that drops out when the track goes quiet.
    constexpr int blockSize = 256;

    ReverbEngine engine;
    engine.setMixProportion (0.5f);
    engine.setDecaySeconds (4.0f);
    engine.setEngineMode (ReverbEngine::EngineMode::hybridTail);
    engine.setTailModMode (FdnTail::ModulationMode::matrix);

    juce::dsp::ProcessSpec spec { testSampleRate, static_cast<juce::uint32> (blockSize), 2 };
    engine.prepare (spec);
    engine.regenerateImpulseResponseIfNeeded();
    REQUIRE (engine.waitForPendingRender());

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::Random random (67);

    const auto timeBlocks = [&] (bool silent)
    {
        juce::ScopedNoDenormals noDenormals;

        // Warm up first so neither measurement pays for cold caches.
        for (int block = 0; block < 50; ++block)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto sample = silent ? 0.0f : (random.nextFloat() * 2.0f - 1.0f);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
            }

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }

        const auto start = juce::Time::getHighResolutionTicks();

        for (int block = 0; block < 400; ++block)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto sample = silent ? 0.0f : (random.nextFloat() * 2.0f - 1.0f);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
            }

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }

        return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    };

    const auto busySeconds = timeBlocks (false);
    const auto silentSeconds = timeBlocks (true);

    INFO ("busy " << busySeconds << " s, silent " << silentSeconds << " s");
    REQUIRE (busySeconds > 0.0);
    CHECK (silentSeconds < busySeconds * 1.2);
}

TEST_CASE ("6.15 Hybrid mode stays well inside its CPU budget", "[dsp][engine][.benchmark]")
{
    constexpr int blockSize = 128;
    constexpr int numBlocks = 2000;

    ReverbEngine engine;
    engine.setMixProportion (0.5f);
    engine.setDecaySeconds (5.0f);
    engine.setEngineMode (ReverbEngine::EngineMode::hybridTail);
    engine.setTailModMode (FdnTail::ModulationMode::matrix);

    juce::dsp::ProcessSpec spec { testSampleRate, static_cast<juce::uint32> (blockSize), 2 };
    engine.prepare (spec);
    engine.regenerateImpulseResponseIfNeeded();
    REQUIRE (engine.waitForPendingRender());

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::Random random (71);

    for (int block = 0; block < 100; ++block)
    {
        buffer.clear();
        juce::dsp::AudioBlock<float> warmUp (buffer);
        engine.process (warmUp);
    }

    const auto start = juce::Time::getHighResolutionTicks();

    for (int block = 0; block < numBlocks; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = random.nextFloat() * 2.0f - 1.0f;
            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    const auto realTimeSeconds = static_cast<double> (numBlocks * blockSize) / testSampleRate;
    const auto load = elapsed / realTimeSeconds;

    // Note this is a Debug build: the release figure is several times lower.
    // The assertion is a smoke test against an accidental order-of-magnitude
    // regression, not the brief's 12% release-build budget.
    INFO ("hybrid load " << (load * 100.0) << "% of real time (Debug build)");
    CHECK (load < 1.0);
}
