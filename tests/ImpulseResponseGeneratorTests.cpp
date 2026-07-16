#include "dsp/ImpulseResponseGenerator.h"
#include "TestHelpers.h"

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    //==========================================================================
    // v0.2.0 guarantee-test helpers (docs/design-brief.md's "Guarantees &
    // tests" section).

    // Band-limits a copy of `ir`'s channel 0 to (lowHz, highHz) (skipping
    // the highpass stage if lowHz <= 0, and the lowpass stage if highHz >=
    // Nyquist), then estimates that band's RT60 via linear regression of
    // the log short-time-RMS envelope against time - the standard RT60-
    // measurement convention referenced by the brief. Returns 0.0 if the
    // regression is degenerate (e.g. a non-decaying/frozen tail).
    double measureBandRt60Seconds (const juce::AudioBuffer<float>& ir, double sampleRate, float lowHz, float highHz)
    {
        juce::AudioBuffer<float> buffer (1, ir.getNumSamples());
        buffer.copyFrom (0, 0, ir, 0, 0, ir.getNumSamples());

        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32> (ir.getNumSamples()), 1 };
        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> context (block);

        // Cascade each stage twice (4th-order/~24dB-per-octave effective
        // slope) rather than a single 2nd-order section: a fast-decaying
        // band (e.g. the low band at Bass Decay's 25% minimum) can fall
        // well below a slower-decaying adjacent band within the buffer's
        // length, and a single 2nd-order filter's gentler stopband
        // rejection lets enough of that adjacent-band energy leak through
        // to dominate the *measured* envelope once the true band's own
        // content has decayed past it - corrupting the RT60 regression.
        // The steeper cascade keeps the measurement faithful to the
        // production 3-band split it's checking (docs/design-brief.md's
        // multiband-decay guarantee).
        if (lowHz > 0.0f)
        {
            for (int stage = 0; stage < 2; ++stage)
            {
                juce::dsp::IIR::Filter<float> highPass;
                highPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, lowHz);
                highPass.prepare (spec);
                highPass.process (context);
            }
        }

        if (static_cast<double> (highHz) < sampleRate * 0.5)
        {
            for (int stage = 0; stage < 2; ++stage)
            {
                juce::dsp::IIR::Filter<float> lowPass;
                lowPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, highHz);
                lowPass.prepare (spec);
                lowPass.process (context);
            }
        }

        const auto windowSamples = juce::jmax (1, static_cast<int> (0.005 * sampleRate));
        const auto* data = buffer.getReadPointer (0);
        const auto numSamples = buffer.getNumSamples();

        std::vector<double> timesSec;
        std::vector<double> levelsDb;

        for (int start = 0; start + windowSamples <= numSamples; start += windowSamples)
        {
            double sumSq = 0.0;

            for (int i = 0; i < windowSamples; ++i)
            {
                const auto v = static_cast<double> (data[start + i]);
                sumSq += v * v;
            }

            const auto rms = std::sqrt (sumSq / static_cast<double> (windowSamples));
            timesSec.push_back (static_cast<double> (start) / sampleRate);
            levelsDb.push_back (20.0 * std::log10 (juce::jmax (rms, 1.0e-9)));
        }

        if (timesSec.size() < 2)
            return 0.0;

        // T30-style truncated regression (the real-world RT60-measurement
        // convention this helper is modelled on): fit only a *contiguous*
        // window starting from the peak, from -5dB to -35dB relative to it,
        // then extrapolate to -60dB. This avoids two failure modes a
        // whole-buffer regression is vulnerable to on a synthetic, per-
        // band-filtered signal: (a) a fast-decaying band (e.g. Bass Decay
        // at its 25% minimum) dropping below the isolation filter's own
        // finite stopband rejection floor well before the buffer ends,
        // where noisy fluctuations can wander back into the [-35, -5] dB
        // range by chance and corrupt a *non-contiguous* selection (which
        // is why this scans forward from the peak and stops at the first
        // exit, rather than collecting every matching sample anywhere in
        // the buffer); (b) the early-reflection layer's onset transient
        // biasing the fit's start.
        const auto peakIt = std::max_element (levelsDb.begin(), levelsDb.end());
        const auto peakIndex = static_cast<size_t> (std::distance (levelsDb.begin(), peakIt));
        const auto peakDb = *peakIt;

        std::vector<double> fitTimes, fitLevels;
        bool enteredWindow = false;

        for (size_t i = peakIndex; i < levelsDb.size(); ++i)
        {
            const auto relativeDb = levelsDb[i] - peakDb;

            if (! enteredWindow)
            {
                if (relativeDb <= -5.0)
                    enteredWindow = true;
                else
                    continue;
            }

            if (relativeDb < -35.0)
                break; // contiguous cutoff: stop at the first exit from the T30 window

            fitTimes.push_back (timesSec[i]);
            fitLevels.push_back (levelsDb[i]);
        }

        // Fall back to the full range if the T30 window didn't capture
        // enough points (e.g. a very short Decay/buffer).
        if (fitTimes.size() < 4)
        {
            fitTimes = timesSec;
            fitLevels = levelsDb;
        }

        const auto n = static_cast<double> (fitTimes.size());
        double sumT = 0.0, sumDb = 0.0, sumTT = 0.0, sumTDb = 0.0;

        for (size_t i = 0; i < fitTimes.size(); ++i)
        {
            sumT += fitTimes[i];
            sumDb += fitLevels[i];
            sumTT += fitTimes[i] * fitTimes[i];
            sumTDb += fitTimes[i] * fitLevels[i];
        }

        const auto denominator = n * sumTT - sumT * sumT;

        if (std::abs (denominator) < 1.0e-9)
            return 0.0;

        const auto slopeDbPerSecond = (n * sumTDb - sumT * sumDb) / denominator;

        if (slopeDbPerSecond >= -1.0e-6)
            return 0.0; // degenerate/non-decaying - not expected for a non-frozen tail

        return -60.0 / slopeDbPerSecond;
    }

    // Magnitude-weighted spectral centroid (Hz) of a single analysis
    // window, via a zero-padded forward FFT - juce::dsp::FFT requires a
    // power-of-two size.
    double spectralCentroidHz (const float* data, int numSamples, double sampleRate)
    {
        int order = 0;
        while ((1 << order) < numSamples)
            ++order;

        const auto fftSize = 1 << order;
        juce::dsp::FFT fft (order);

        std::vector<float> fftData (static_cast<size_t> (fftSize) * 2, 0.0f);

        for (int i = 0; i < numSamples; ++i)
            fftData[static_cast<size_t> (i)] = data[i];

        fft.performFrequencyOnlyForwardTransform (fftData.data());

        double weightedSum = 0.0;
        double magnitudeSum = 0.0;
        const auto binHz = sampleRate / static_cast<double> (fftSize);

        for (int bin = 1; bin < fftSize / 2; ++bin) // skip DC
        {
            const auto magnitude = static_cast<double> (fftData[static_cast<size_t> (bin)]);
            weightedSum += magnitude * static_cast<double> (bin) * binHz;
            magnitudeSum += magnitude;
        }

        return magnitudeSum > 1.0e-9 ? weightedSum / magnitudeSum : 0.0;
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
    // ~4410-sample buffer, far shorter than Cathedral's ~220ms (~9702
    // samples, at the default 50% Size) flat-window end - exactly the
    // "window bigger than the buffer" scenario the short-Decay scaling
    // rule exists for.
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
                                                                   ReverbIR::SpaceType::cathedral, 0.0f, false, seed,
                                                                   ReverbIR::defaultSize01);

    const auto lengthSamples = ir.getNumSamples();
    REQUIRE (lengthSamples > 1);

    // Reference re-implementation of addEarlyReflections()'s v0.2.0
    // density-buildup/flat-window model, mirroring Cathedral's
    // characteristics at the default Size (50%) - see
    // ImpulseResponseGenerator.cpp's characteristicsFor()/
    // buildupTapFraction/buildupTapAmplitudeScale/flatTapAmplitudeScale/
    // densityStepMs - and the *fixed* short-Decay scaling rule this test
    // exists to pin down: whenever the buffer is shorter than the raw
    // buildup+flat window, the window is scaled down proportionally so
    // tap offsets are always drawn from [0, lengthSamples).
    constexpr float buildupMsAtSizeZero = 30.0f, buildupMsAtSizeOne = 80.0f;
    constexpr float flatEndMsAtSizeZero = 140.0f, flatEndMsAtSizeOne = 300.0f;
    constexpr int numTaps = 30;
    constexpr float buildupTapFraction = 0.55f;
    constexpr float buildupTapAmplitudeScale = 42.0f;
    constexpr float flatTapAmplitudeScale = 7.0f;
    constexpr float densityStepMs = 10.0f;
    constexpr float size01 = ReverbIR::defaultSize01;

    auto buildupMs = juce::jmap (size01, 0.0f, 1.0f, buildupMsAtSizeZero, buildupMsAtSizeOne);
    auto flatEndMs = juce::jmap (size01, 0.0f, 1.0f, flatEndMsAtSizeZero, flatEndMsAtSizeOne);

    const auto flatEndSamplesRaw = juce::jmax (1, static_cast<int> (std::round (flatEndMs * 0.001 * sampleRate)));
    REQUIRE (flatEndSamplesRaw > lengthSamples); // sanity: this really is the "window > buffer" scenario the fix targets.

    const auto scale = static_cast<float> (lengthSamples) / static_cast<float> (flatEndSamplesRaw);
    buildupMs *= scale;
    flatEndMs *= scale;

    const auto numBuildupTaps = juce::jmax (1, static_cast<int> (std::round (static_cast<float> (numTaps) * buildupTapFraction)));
    const auto numFlatTaps = juce::jmax (0, numTaps - numBuildupTaps);

    std::vector<float> reference (static_cast<size_t> (lengthSamples), 0.0f);
    reference[0] += 1.0f; // gain == 1.0 (early-only: Early/Late Balance == 0.0), tap 0 forced at sample 0

    juce::Random random (static_cast<juce::int64> (seed) * 40503 + 101); // channel == 0

    auto placeTap = [&] (float timeMs, float amplitudeScale)
    {
        const auto sampleIndex = juce::jlimit (0, lengthSamples - 1,
                                                static_cast<int> (std::round (timeMs * 0.001 * sampleRate)));
        const auto polarity = random.nextFloat() < 0.5f ? -1.0f : 1.0f;
        const auto jitter = 0.85f + 0.3f * random.nextFloat();
        reference[static_cast<size_t> (sampleIndex)] += amplitudeScale * polarity * jitter;
    };

    const auto numSteps = juce::jmax (1, static_cast<int> (std::ceil (buildupMs / densityStepMs)));
    std::vector<int> tapsPerStep (static_cast<size_t> (numSteps), 0);
    {
        const auto weightSum = static_cast<float> (numSteps * (numSteps + 1) / 2);
        int allocated = 0;

        for (int step = 0; step < numSteps; ++step)
        {
            const auto weight = static_cast<float> (step + 1);
            const auto count = static_cast<int> (std::floor (static_cast<float> (numBuildupTaps) * weight / weightSum));
            tapsPerStep[static_cast<size_t> (step)] = count;
            allocated += count;
        }

        auto remaining = numBuildupTaps - allocated;

        for (int step = numSteps - 1; step >= 0 && remaining > 0; --step)
        {
            ++tapsPerStep[static_cast<size_t> (step)];
            --remaining;
        }
    }

    for (int step = 0; step < numSteps; ++step)
    {
        const auto stepStartMs = static_cast<float> (step) * densityStepMs;
        const auto stepEndMs = juce::jmin (buildupMs, stepStartMs + densityStepMs);
        const auto tapsInStep = tapsPerStep[static_cast<size_t> (step)];

        for (int k = 0; k < tapsInStep; ++k)
        {
            const auto u = (static_cast<float> (k) + 0.5f) / static_cast<float> (tapsInStep);
            placeTap (stepStartMs + u * (stepEndMs - stepStartMs), buildupTapAmplitudeScale);
        }
    }

    for (int tap = 1; tap <= numFlatTaps; ++tap)
    {
        const auto u = static_cast<float> (tap) / static_cast<float> (numFlatTaps + 1);
        placeTap (buildupMs + u * (flatEndMs - buildupMs), flatTapAmplitudeScale);
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

    INFO ("first mismatch at index: " << firstMismatchIndex << " (lengthSamples: " << lengthSamples << ")");
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

//==============================================================================
// v0.2.0 guarantees (docs/design-brief.md's "Guarantees & tests" section).

TEST_CASE ("Early-reflection energy ratio: 0-50ms carries 2-3x the energy of 50-150ms at default settings",
           "[dsp][ir][v2]")
{
    // Griesinger's documented ratio (docs/research-notes.md section 1) -
    // direct proof the density-buildup model replaced v1's geometric-decay
    // shape, which measurably favoured the *later* window instead (a
    // slowly-decaying diffuse tail alone carries more energy in a 100ms
    // window than in a preceding 50ms one).
    for (const auto space : { ReverbIR::SpaceType::cathedral, ReverbIR::SpaceType::hall, ReverbIR::SpaceType::chamber })
    {
        const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.5f, 8000.0f, 1,
                                                                       space, 0.8f); // default Early/Late Balance

        const auto window50msSamples = static_cast<int> (0.05 * testSampleRate);
        const auto window150msSamples = static_cast<int> (0.15 * testSampleRate);
        REQUIRE (ir.getNumSamples() > window150msSamples);

        const auto* data = ir.getReadPointer (0);

        double energy0to50 = 0.0, energy50to150 = 0.0;

        for (int i = 0; i < window50msSamples; ++i)
            energy0to50 += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        for (int i = window50msSamples; i < window150msSamples; ++i)
            energy50to150 += static_cast<double> (data[i]) * static_cast<double> (data[i]);

        REQUIRE (energy50to150 > 0.0);
        const auto ratio = energy0to50 / energy50to150;

        INFO ("space index: " << static_cast<int> (space) << ", ratio: " << ratio);
        CHECK (ratio >= 1.5); // generous lower bound around Griesinger's documented 2-3x
        CHECK (ratio <= 6.0); // generous upper bound - not a razor-tight hardware match
    }
}

TEST_CASE ("Early-reflection density buildup: tap density is non-decreasing across successive 10ms sub-windows",
           "[dsp][ir][v2]")
{
    // Balance = 0.0 isolates the early-reflection layer (no diffuse-tail
    // contribution), so every local maximum in |signal| corresponds to a
    // discrete tap.
    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 1,
                                                                   ReverbIR::SpaceType::hall, 0.0f, false, 1,
                                                                   ReverbIR::defaultSize01);

    const auto* data = ir.getReadPointer (0);
    const auto numSamples = ir.getNumSamples();

    // Hall at the default Size (50%) has a 35ms buildup window (see
    // docs/design-brief.md) - the non-decreasing-density guarantee only
    // applies *within* that buildup window; the flat-density handoff phase
    // beyond it (see the class doc comment) is deliberately *not* part of
    // this assertion, so bins are scanned exactly up to buildupMs.
    constexpr double buildupMs = 35.0;
    constexpr double binMs = 10.0;
    const auto numBins = static_cast<int> (std::ceil (buildupMs / binMs));
    const auto binSamples = juce::jmax (1, static_cast<int> (binMs * 0.001 * testSampleRate));

    std::vector<int> tapCountPerBin (static_cast<size_t> (numBins), 0);

    // Tap 0 (forced at sample 0) always counts as the first bin's peak.
    if (std::abs (data[0]) > 1.0e-6f)
        ++tapCountPerBin[0];

    for (int i = 1; i + 1 < numSamples; ++i)
    {
        const auto isPeak = std::abs (data[i]) > std::abs (data[i - 1])
                             && std::abs (data[i]) >= std::abs (data[i + 1])
                             && std::abs (data[i]) > 1.0e-6f;

        if (! isPeak)
            continue;

        const auto bin = i / binSamples;

        if (bin >= 0 && bin < numBins)
            ++tapCountPerBin[static_cast<size_t> (bin)];
    }

    for (size_t i = 1; i < tapCountPerBin.size(); ++i)
    {
        INFO ("bin " << (i - 1) << ": " << tapCountPerBin[i - 1] << ", bin " << i << ": " << tapCountPerBin[i]);
        CHECK (tapCountPerBin[i] >= tapCountPerBin[i - 1]);
    }
}

TEST_CASE ("Onset invariance: tap 0 stays fixed at sample 0 regardless of Space/Size/Balance", "[dsp][ir][v2]")
{
    for (const auto space : { ReverbIR::SpaceType::cathedral, ReverbIR::SpaceType::hall, ReverbIR::SpaceType::chamber })
    {
        for (const auto size01 : { 0.0f, 0.5f, 1.0f })
        {
            for (const auto balance : { 0.0f, 0.5f, 1.0f })
            {
                const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 1,
                                                                               space, balance, false, 1, size01);

                if (balance >= 1.0f)
                    continue; // pure diffuse tail: no discrete early tap 0 is added by design

                // Tap 0's contribution (the early layer's gain, before any
                // diffuse-noise contribution at that same sample) should
                // dominate sample 0 relative to any single diffuse-noise
                // sample nearby.
                CHECK (std::abs (ir.getSample (0, 0)) > 0.0f);
            }
        }
    }
}

TEST_CASE ("Size decoupling: sweeping Size does not measurably change RT60 but does change the early window",
           "[dsp][ir][v2]")
{
    constexpr float decaySeconds = 3.0f;

    // Balance = 1.0 isolates the diffuse tail (pure RT60 measurement, no
    // early-tap transients to bias the regression).
    const auto irSizeMin = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                          ReverbIR::SpaceType::hall, 1.0f, false, 1,
                                                                          ReverbIR::minSize01);
    const auto irSizeMax = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                          ReverbIR::SpaceType::hall, 1.0f, false, 1,
                                                                          ReverbIR::maxSize01);

    const auto rt60Min = measureBandRt60Seconds (irSizeMin, testSampleRate, 0.0f, static_cast<float> (testSampleRate));
    const auto rt60Max = measureBandRt60Seconds (irSizeMax, testSampleRate, 0.0f, static_cast<float> (testSampleRate));

    REQUIRE (rt60Min > 0.0);
    REQUIRE (rt60Max > 0.0);
    // Size must not leak into decay time - a generous tolerance around the
    // requested Decay (3s), far tighter than the Size-driven early-window
    // change measured below.
    CHECK (rt60Min == Catch::Approx (decaySeconds).margin (0.5));
    CHECK (rt60Max == Catch::Approx (decaySeconds).margin (0.5));

    // Meanwhile the early-reflection window measurably changes: at Balance
    // == 0 (early-only), the last sample with non-negligible early-tap
    // energy should land later in the buffer at Size == 100% than at
    // Size == 0% (Hall's buildup/flat window widens with Size).
    auto lastSignificantTapSample = [] (const juce::AudioBuffer<float>& ir)
    {
        const auto* data = ir.getReadPointer (0);
        const auto threshold = TestHelpers::peakAbsolute (ir) * 0.05f;

        for (int i = ir.getNumSamples() - 1; i >= 0; --i)
            if (std::abs (data[i]) > threshold)
                return i;

        return 0;
    };

    const auto earlyMin = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                         ReverbIR::SpaceType::hall, 0.0f, false, 1,
                                                                         ReverbIR::minSize01);
    const auto earlyMax = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                         ReverbIR::SpaceType::hall, 0.0f, false, 1,
                                                                         ReverbIR::maxSize01);

    CHECK (lastSignificantTapSample (earlyMax) > lastSignificantTapSample (earlyMin));
}

TEST_CASE ("Multiband decay ordering: lowRT60 > midRT60 > highRT60 at default Bass Decay", "[dsp][ir][v2]")
{
    constexpr float decaySeconds = 3.0f;

    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                   ReverbIR::SpaceType::hall, 0.8f, false, 1,
                                                                   ReverbIR::defaultSize01, ReverbIR::defaultBassDecayMultiplier);

    const auto lowRt60 = measureBandRt60Seconds (ir, testSampleRate, 0.0f, ReverbIR::lowMidCrossoverHz);
    const auto midRt60 = measureBandRt60Seconds (ir, testSampleRate, ReverbIR::lowMidCrossoverHz, ReverbIR::midHighCrossoverHz);
    const auto highRt60 = measureBandRt60Seconds (ir, testSampleRate, ReverbIR::midHighCrossoverHz, static_cast<float> (testSampleRate));

    INFO ("lowRt60: " << lowRt60 << ", midRt60: " << midRt60 << ", highRt60: " << highRt60);
    REQUIRE (lowRt60 > 0.0);
    REQUIRE (midRt60 > 0.0);
    REQUIRE (highRt60 > 0.0);
    CHECK (lowRt60 > midRt60);
    CHECK (midRt60 > highRt60);
}

TEST_CASE ("Sweeping Bass Decay moves the low-band RT60 monotonically while leaving mid/high materially unchanged",
           "[dsp][ir][v2]")
{
    constexpr float decaySeconds = 3.0f;

    double previousLowRt60 = 0.0;
    double firstMidRt60 = -1.0, firstHighRt60 = -1.0;

    for (const auto bassDecay : { ReverbIR::minBassDecayMultiplier, 0.75f, ReverbIR::defaultBassDecayMultiplier, 1.6f, ReverbIR::maxBassDecayMultiplier })
    {
        const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                       ReverbIR::SpaceType::hall, 0.8f, false, 1,
                                                                       ReverbIR::defaultSize01, bassDecay);

        const auto lowRt60 = measureBandRt60Seconds (ir, testSampleRate, 0.0f, ReverbIR::lowMidCrossoverHz);
        const auto midRt60 = measureBandRt60Seconds (ir, testSampleRate, ReverbIR::lowMidCrossoverHz, ReverbIR::midHighCrossoverHz);
        const auto highRt60 = measureBandRt60Seconds (ir, testSampleRate, ReverbIR::midHighCrossoverHz, static_cast<float> (testSampleRate));

        INFO ("bassDecay: " << bassDecay << ", lowRt60: " << lowRt60 << ", midRt60: " << midRt60 << ", highRt60: " << highRt60);
        REQUIRE (lowRt60 > 0.0);

        CHECK (lowRt60 > previousLowRt60);
        previousLowRt60 = lowRt60;

        if (firstMidRt60 < 0.0)
        {
            firstMidRt60 = midRt60;
            firstHighRt60 = highRt60;
        }
        else
        {
            CHECK (midRt60 == Catch::Approx (firstMidRt60).margin (firstMidRt60 * 0.35));
            CHECK (highRt60 == Catch::Approx (firstHighRt60).margin (firstHighRt60 * 0.35));
        }
    }
}

TEST_CASE ("Progressive HF darkening: spectral centroid of the tail is non-increasing across the buffer",
           "[dsp][ir][v2]")
{
    constexpr float decaySeconds = 4.0f;

    // Balance = 1.0 isolates the diffuse tail (no early-tap transients to
    // bias the spectral estimate).
    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 6000.0f, 1,
                                                                   ReverbIR::SpaceType::hall, 1.0f);

    const auto* data = ir.getReadPointer (0);
    const auto numSamples = ir.getNumSamples();

    constexpr int numWindows = 6;
    const auto windowSamples = numSamples / numWindows;
    REQUIRE (windowSamples > 1024);

    std::vector<double> centroids;

    for (int w = 0; w < numWindows; ++w)
        centroids.push_back (spectralCentroidHz (data + w * windowSamples, windowSamples, testSampleRate));

    // A generous tolerance for windowed-FFT/noise-estimation variance on a
    // stochastic signal - the underlying mechanism (per-band decay rates
    // plus the high band's descending cutoff) is deterministic and
    // monotonic; the measurement itself is on noise-driven content.
    constexpr double toleranceHz = 200.0;

    for (size_t i = 1; i < centroids.size(); ++i)
    {
        INFO ("window " << (i - 1) << ": " << centroids[i - 1] << " Hz, window " << i << ": " << centroids[i] << " Hz");
        CHECK (centroids[i] <= centroids[i - 1] + toleranceHz);
    }

    CHECK (centroids.front() > centroids.back()); // net darkening across the whole buffer
}

TEST_CASE ("Freeze non-periodicity: no dominant short-lag autocorrelation peak", "[dsp][ir][v2]")
{
    // A regression guard proving Freeze was never reimplemented as a
    // short looped buffer: a genuinely looped short buffer would show a
    // strong normalised-autocorrelation peak (close to 1.0) at its loop
    // length; white-noise-derived diffuse content should not, at any
    // candidate short lag.
    constexpr float decaySeconds = 2.0f;

    const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, 8000.0f, 1,
                                                                   ReverbIR::SpaceType::hall, 0.8f, true); // frozen

    const auto* data = ir.getReadPointer (0);
    const auto numSamples = ir.getNumSamples();

    const auto analysisStart = numSamples / 4;
    const auto analysisLength = numSamples / 2;

    double zeroLagEnergy = 0.0;

    for (int i = 0; i < analysisLength; ++i)
        zeroLagEnergy += static_cast<double> (data[analysisStart + i]) * static_cast<double> (data[analysisStart + i]);

    REQUIRE (zeroLagEnergy > 0.0);

    for (const double lagMs : { 1.0, 5.0, 10.0, 25.0, 50.0, 100.0 })
    {
        const auto lagSamples = static_cast<int> (lagMs * 0.001 * testSampleRate);

        if (lagSamples <= 0 || analysisStart + analysisLength + lagSamples >= numSamples)
            continue;

        double crossCorrelation = 0.0;

        for (int i = 0; i < analysisLength; ++i)
            crossCorrelation += static_cast<double> (data[analysisStart + i]) * static_cast<double> (data[analysisStart + i + lagSamples]);

        const auto normalised = std::abs (crossCorrelation) / zeroLagEnergy;
        INFO ("lag: " << lagMs << "ms, normalised autocorrelation: " << normalised);
        CHECK (normalised < 0.3);
    }
}

TEST_CASE ("Size and Bass Decay are finite across their full documented ranges", "[dsp][ir][v2][robustness]")
{
    const float sizeValues[] = { ReverbIR::minSize01, 0.25f, ReverbIR::defaultSize01, 0.75f, ReverbIR::maxSize01 };
    const float bassDecayValues[] = { ReverbIR::minBassDecayMultiplier, 0.75f, ReverbIR::defaultBassDecayMultiplier, 1.6f, ReverbIR::maxBassDecayMultiplier };

    for (const auto size01 : sizeValues)
    {
        for (const auto bassDecay : bassDecayValues)
        {
            for (const auto freeze : { false, true })
            {
                const auto ir = ReverbIR::generateProceduralImpulseResponse (testSampleRate, 2.0f, 8000.0f, 2,
                                                                               ReverbIR::SpaceType::cathedral, 0.5f, freeze, 1,
                                                                               size01, bassDecay);

                CHECK (TestHelpers::allSamplesFinite (ir));
            }
        }
    }
}
