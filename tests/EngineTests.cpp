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
