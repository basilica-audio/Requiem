#include "dsp/ImpulseResponseGenerator.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace
{
    constexpr double testSampleRate = 48000.0;

    double sumOfSquares (const juce::AudioBuffer<float>& buffer)
    {
        double total = 0.0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                total += static_cast<double> (data[sample]) * static_cast<double> (data[sample]);
        }

        return total;
    }
}

TEST_CASE ("Generated impulse response is finite for the full parameter range", "[dsp][ir][robustness]")
{
    const float decayValues[] = { ReverbIR::minDecaySeconds, 0.5f, 1.0f, 2.5f, 5.0f, ReverbIR::maxDecaySeconds };
    const float dampingValues[] = { ReverbIR::minDampingHz, 500.0f, 2000.0f, 8000.0f, ReverbIR::maxDampingHz };

    for (const auto decay : decayValues)
    {
        for (const auto damping : dampingValues)
        {
            const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decay, damping, 2);

            CHECK (ir.getNumChannels() == 2);
            CHECK (ir.getNumSamples() > 0);
            CHECK (TestHelpers::allSamplesFinite (ir));
        }
    }
}

TEST_CASE ("Generated impulse response is finite at extreme/unusual sample rates", "[dsp][ir][robustness]")
{
    for (const double sampleRate : { 8000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto ir = ReverbIR::generateProceduralImpulseResponse (sampleRate, 2.0f, 8000.0f, 2);

        CHECK (TestHelpers::allSamplesFinite (ir));
    }
}

TEST_CASE ("Mono and stereo channel counts are honoured and clamped", "[dsp][ir]")
{
    CHECK (ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 1).getNumChannels() == 1);
    CHECK (ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 2).getNumChannels() == 2);

    // Out-of-range channel counts are clamped to [1, 2] rather than
    // crashing or producing a degenerate (0-channel) buffer.
    CHECK (ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 0).getNumChannels() == 1);
    CHECK (ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 8).getNumChannels() == 2);
}

TEST_CASE ("Larger Decay produces a longer impulse response with more total tail energy", "[dsp][ir]")
{
    // Same seed (default) and Damping for both calls: the only difference
    // is Decay, so any difference in outcome is attributable to it alone.
    const auto shortIr = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 2);
    const auto longIr = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 5.0f, 8000.0f, 2);

    CHECK (longIr.getNumSamples() > shortIr.getNumSamples());
    CHECK (sumOfSquares (longIr) > sumOfSquares (shortIr));
}

TEST_CASE ("Decay is clamped to the documented [min, max] range", "[dsp][ir]")
{
    const auto belowMin = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 0.0f, 8000.0f, 2);
    const auto atMin = ReverbIR::generateProceduralImpulseResponse (testSampleRate, ReverbIR::minDecaySeconds, 8000.0f, 2);
    CHECK (belowMin.getNumSamples() == atMin.getNumSamples());

    const auto aboveMax = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1000.0f, 8000.0f, 2);
    const auto atMax = ReverbIR::generateProceduralImpulseResponse (testSampleRate, ReverbIR::maxDecaySeconds, 8000.0f, 2);
    CHECK (aboveMax.getNumSamples() == atMax.getNumSamples());
}

TEST_CASE ("Left and right channels are decorrelated (stereo width in the tail)", "[dsp][ir]")
{
    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 2);

    REQUIRE (ir.getNumChannels() == 2);

    bool anyDifferent = false;

    for (int i = 0; i < ir.getNumSamples(); ++i)
    {
        if (std::abs (ir.getSample (0, i) - ir.getSample (1, i)) > 1.0e-12f)
        {
            anyDifferent = true;
            break;
        }
    }

    CHECK (anyDifferent);
}

TEST_CASE ("Generated impulse response is finite across Space/Early-Late-Balance/Freeze combinations", "[dsp][ir][robustness]")
{
    const ReverbIR::SpaceType spaces[] = { ReverbIR::SpaceType::cathedral, ReverbIR::SpaceType::hall, ReverbIR::SpaceType::chamber };
    const float balances[] = { 0.0f, 0.3f, 0.8f, 1.0f };
    const float decayValues[] = { ReverbIR::minDecaySeconds, 0.5f, ReverbIR::maxDecaySeconds };

    for (const auto space : spaces)
    {
        for (const auto balance : balances)
        {
            for (const auto freeze : { false, true })
            {
                for (const auto decay : decayValues)
                {
                    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decay, 8000.0f, 2,
                                                                                  space, balance, freeze);

                    CHECK (ir.getNumChannels() == 2);
                    CHECK (ir.getNumSamples() > 0);
                    CHECK (TestHelpers::allSamplesFinite (ir));
                }
            }
        }
    }
}

TEST_CASE ("Space affects the generated impulse response (Cathedral/Hall/Chamber differ)", "[dsp][ir]")
{
    const auto cathedral = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 2, ReverbIR::SpaceType::cathedral, 0.0f);
    const auto hall = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 2, ReverbIR::SpaceType::hall, 0.0f);
    const auto chamber = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 2, ReverbIR::SpaceType::chamber, 0.0f);

    // Early-Late Balance == 0.0 isolates the early-reflection layer (no
    // diffuse tail contribution), so any difference between spaces here is
    // attributable to Space's effect on that layer alone.
    bool cathedralDiffersFromChamber = false;

    for (int i = 0; i < cathedral.getNumSamples(); ++i)
    {
        if (std::abs (cathedral.getSample (0, i) - chamber.getSample (0, i)) > 1.0e-9f)
        {
            cathedralDiffersFromChamber = true;
            break;
        }
    }

    CHECK (cathedralDiffersFromChamber);
    CHECK (TestHelpers::peakAbsolute (hall) > 0.0f); // sanity: generation actually produced non-zero data
}

TEST_CASE ("Early/Late Balance = 0 isolates early reflections; = 1 isolates the diffuse tail", "[dsp][ir]")
{
    // At balance == 1.0 (pure diffuse tail), the buffer's very first sample
    // is whatever the one-pole-filtered noise happens to be there - no
    // discrete "tap 0 at sample 0" contribution should be added.
    const auto pureLate = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 20000.0f, 1,
                                                                        ReverbIR::SpaceType::hall, 1.0f);
    const auto pureEarly = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 20000.0f, 1,
                                                                         ReverbIR::SpaceType::hall, 0.0f);

    // Both are finite and non-degenerate.
    CHECK (TestHelpers::allSamplesFinite (pureLate));
    CHECK (TestHelpers::allSamplesFinite (pureEarly));

    // The early-only IR's total energy should be concentrated far more
    // tightly near the start of the buffer than the late-only IR's, since
    // the early layer is a handful of discrete taps within a short window
    // while the late layer is continuous filtered noise across the whole
    // buffer.
    const auto quarterLength = pureLate.getNumSamples() / 4;

    auto energyInFirstQuarter = [] (const juce::AudioBuffer<float>& buffer, int length)
    {
        double energy = 0.0;
        const auto* data = buffer.getReadPointer (0);

        for (int i = 0; i < length; ++i)
            energy += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        return energy;
    };

    auto totalEnergy = [] (const juce::AudioBuffer<float>& buffer)
    {
        double energy = 0.0;
        const auto* data = buffer.getReadPointer (0);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            energy += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        return energy;
    };

    const auto earlyFraction = energyInFirstQuarter (pureEarly, quarterLength) / totalEnergy (pureEarly);
    const auto lateFraction = energyInFirstQuarter (pureLate, quarterLength) / totalEnergy (pureLate);

    CHECK (earlyFraction > lateFraction);
}

TEST_CASE ("Freeze produces a sustained (non-decaying) tail", "[dsp][ir]")
{
    constexpr float decaySeconds = 1.0f;

    const auto normal = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                       ReverbIR::SpaceType::hall, 0.8f, false);
    const auto frozen = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                       ReverbIR::SpaceType::hall, 0.8f, true);

    REQUIRE (normal.getNumSamples() == frozen.getNumSamples());
    CHECK (TestHelpers::allSamplesFinite (frozen));

    // Compare the energy in the last 10% of the buffer against the first
    // 10%: a normal (RT60-decaying) tail should be dramatically quieter at
    // the end than the start (~-60 dB by construction), while a frozen
    // (flat-envelope) tail should stay roughly comparable throughout.
    const auto tenPercent = juce::jmax (1, normal.getNumSamples() / 10);
    const auto tailStart = normal.getNumSamples() - tenPercent;

    auto rmsRange = [] (const juce::AudioBuffer<float>& buffer, int start, int length)
    {
        double sumOfSquares = 0.0;
        const auto* data = buffer.getReadPointer (0);

        for (int i = start; i < start + length; ++i)
            sumOfSquares += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        return std::sqrt (sumOfSquares / static_cast<double> (length));
    };

    const auto normalHeadRms = rmsRange (normal, 0, tenPercent);
    const auto normalTailRms = rmsRange (normal, tailStart, tenPercent);
    const auto frozenHeadRms = rmsRange (frozen, 0, tenPercent);
    const auto frozenTailRms = rmsRange (frozen, tailStart, tenPercent);

    REQUIRE (normalHeadRms > 0.0);
    REQUIRE (frozenHeadRms > 0.0);

    // Normal tail: pronounced decay (tail well below head).
    CHECK (normalTailRms < normalHeadRms * 0.5);
    // Frozen tail: no pronounced decay (tail stays within the same order of
    // magnitude as the head) - and, in relative terms, decays far less than
    // the non-frozen tail does.
    CHECK (frozenTailRms > normalTailRms);
    CHECK (frozenTailRms / frozenHeadRms > normalTailRms / normalHeadRms);
}

TEST_CASE ("Early reflections at very short Decay use a window scaled to the buffer length, not the raw Space window",
           "[dsp][ir]")
{
    // Decay = 0.1 s (the documented minimum) at 44.1 kHz produces a
    // ~4410-sample buffer, far shorter than Cathedral's 150 ms (~6615
    // samples) early-reflection window - exactly the "window bigger than
    // the buffer" scenario from issue #11 item 3.
    constexpr double sampleRate = 44100.0;
    constexpr float decaySeconds = ReverbIR::minDecaySeconds;
    constexpr int seed = 1;

    // Balance = 0.0 forces the diffuse late-tail layer's gain to exactly
    // zero (generateProceduralImpulseResponse(): lateGain = sin(0 *
    // halfPi) == 0), so the returned buffer contains *only* the early-
    // reflection layer's contribution. That lets this test compare
    // directly against a from-scratch reference re-implementation of
    // addEarlyReflections() below without also having to reproduce the
    // (already well-covered elsewhere, e.g. "Left and right channels are
    // decorrelated") filtered-noise diffuse-tail algorithm.
    const auto ir = ReverbIR::generateProceduralImpulseResponse (sampleRate, decaySeconds, 8000.0f, 1,
                                                                   ReverbIR::SpaceType::cathedral, 0.0f, false, seed);

    const auto lengthSamples = ir.getNumSamples();
    REQUIRE (lengthSamples > 1);
    REQUIRE (lengthSamples < 6615); // sanity: this really is the "window > buffer" scenario the fix targets.

    // Reference re-implementation of addEarlyReflections(), mirroring
    // Cathedral's documented characteristics (150 ms window, 22 taps, 0.85
    // per-tap decay factor - see ImpulseResponseGenerator.cpp's
    // characteristicsFor()), the documented seed formula for the
    // early-reflection Random stream (ImpulseResponseGenerator.cpp:
    // `seed * 40503 + channel * 6151 + 101`, channel 0 here), and the
    // *fixed* windowing rule this test exists to pin down: the effective
    // window is scaled down to the buffer length whenever the buffer is
    // shorter than the raw Space window, so tap offsets are always drawn
    // from [0, lengthSamples) and (bar an astronomically unlikely exact
    // draw) never collapse onto a single clamped index. Before the fix,
    // production code used the raw (unscaled) window here instead, so this
    // reference would diverge from its output for any seed/Decay/Space
    // combination where the raw window exceeds the buffer - as this one
    // does.
    constexpr float earlyWindowMs = 150.0f;
    constexpr int numTaps = 22;
    constexpr float tapDecayFactor = 0.85f;

    const auto rawWindowSamples = juce::jmax (1, static_cast<int> (std::round (earlyWindowMs * 0.001 * sampleRate)));
    const auto effectiveWindowSamples = juce::jmin (rawWindowSamples, lengthSamples);

    std::vector<float> reference (static_cast<size_t> (lengthSamples), 0.0f);
    juce::Random random (static_cast<juce::int64> (seed) * 40503 + 101); // channel == 0

    float tapAmplitude = 1.0f; // gain == 1.0 (early-only: Early/Late Balance == 0.0)

    for (int tap = 0; tap < numTaps; ++tap)
    {
        if (tap == 0)
        {
            reference[0] += tapAmplitude;
        }
        else
        {
            const auto sampleIndex = juce::jlimit (0, lengthSamples - 1,
                                                    static_cast<int> (random.nextFloat() * static_cast<float> (effectiveWindowSamples)));
            const auto polarity = random.nextFloat() < 0.5f ? -1.0f : 1.0f;
            const auto jitter = 0.85f + 0.3f * random.nextFloat();

            reference[static_cast<size_t> (sampleIndex)] += tapAmplitude * polarity * jitter;
        }

        tapAmplitude *= tapDecayFactor;
    }

    const auto* data = ir.getReadPointer (0);
    int firstMismatchIndex = -1;

    for (int i = 0; i < lengthSamples; ++i)
    {
        if (std::abs (data[i] - reference[static_cast<size_t> (i)]) > 1.0e-6f)
        {
            firstMismatchIndex = i;
            break;
        }
    }

    INFO ("first mismatch at index: " << firstMismatchIndex << " (lengthSamples: " << lengthSamples
          << ", rawWindowSamples: " << rawWindowSamples << ", effectiveWindowSamples: " << effectiveWindowSamples << ")");
    CHECK (firstMismatchIndex == -1);
}

TEST_CASE ("Freeze suppresses the early-reflection layer and ignores Early/Late Balance", "[dsp][ir]")
{
    // While frozen, the diffuse layer always plays at full gain and the
    // early-reflection layer is always suppressed, regardless of
    // Early/Late Balance - a frozen pad is always the full sustained
    // diffuse wash, never a sustained early-reflection pattern. Generating
    // with balance at both extremes should therefore produce bit-identical
    // output.
    const auto frozenBalanceZero = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 1,
                                                                                  ReverbIR::SpaceType::hall, 0.0f, true);
    const auto frozenBalanceOne = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 1.0f, 8000.0f, 1,
                                                                                 ReverbIR::SpaceType::hall, 1.0f, true);

    REQUIRE (frozenBalanceZero.getNumSamples() == frozenBalanceOne.getNumSamples());
    CHECK (TestHelpers::peakAbsolute (frozenBalanceZero) > 0.0f); // sanity: not silent

    bool identical = true;

    for (int i = 0; i < frozenBalanceZero.getNumSamples(); ++i)
    {
        if (std::abs (frozenBalanceZero.getSample (0, i) - frozenBalanceOne.getSample (0, i)) > 0.0f)
        {
            identical = false;
            break;
        }
    }

    CHECK (identical);
}
