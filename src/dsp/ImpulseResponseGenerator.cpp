#include "ImpulseResponseGenerator.h"

#include <cmath>
#include <vector>

namespace ReverbIR
{
    namespace
    {
        // One-pole low-pass coefficient (RC/exponential-smoothing form):
        // higher cutoffHz -> smaller `a` -> less smoothing -> brighter.
        float onePoleCoefficient (float cutoffHz, double sampleRate) noexcept
        {
            return std::exp (-juce::MathConstants<float>::twoPi * cutoffHz / static_cast<float> (sampleRate));
        }

        // Early-reflection buildup/flat-window timing per Space, scaled
        // continuously by Size within each Space's own envelope (see
        // ImpulseResponseGenerator.h's Size doc comment). At size01 == 0.5,
        // Hall's buildupMs/flatEndMs land exactly on the brief's
        // research-derived defaults (35 ms / 160 ms) - see
        // docs/design-brief.md. Exact per-Space millisecond tables are
        // reasoned (no source publishes them for a generic "Cathedral/Hall/
        // Chamber" 3-way) - the *shape* (buildup + flat window, not
        // geometric decay) is what's sourced.
        struct SpaceCharacteristics
        {
            float buildupMsAtSizeZero;
            float buildupMsAtSizeOne;
            float flatEndMsAtSizeZero;
            float flatEndMsAtSizeOne;
            int numTaps; // discrete taps beyond the fixed tap 0
        };

        SpaceCharacteristics characteristicsFor (SpaceType space)
        {
            switch (space)
            {
                case SpaceType::cathedral: return { 30.0f, 80.0f, 140.0f, 300.0f, 30 };
                case SpaceType::chamber:   return { 10.0f, 25.0f, 60.0f, 120.0f, 10 };
                case SpaceType::hall:
                default:                   return { 20.0f, 50.0f, 100.0f, 220.0f, 18 };
            }
        }

        // Fraction of the discrete tap budget spent on the density-buildup
        // phase (the remainder is spent on the flat-density handoff phase) -
        // a fixed, reasoned split shared by every Space.
        constexpr float buildupTapFraction = 0.55f;

        // Relative tap amplitude (as a fraction of `gain`, the early
        // layer's overall crossfade gain) for the two phases: buildup taps
        // stay close to tap 0's own scale (a dense, energetic cluster of
        // reflections), then step down to a quieter, roughly constant
        // plateau for the flat-density handoff phase. Amplitude is roughly
        // flat *within* each phase (only a step down between the two
        // phases) - this is what produces the measured "0-50ms carries
        // 2-3x the energy of 50-150ms" Griesinger ratio (see the dedicated
        // guarantee test) without reproducing v1's defect of a single loud
        // tap 0 that decays away geometrically.
        constexpr float buildupTapAmplitudeScale = 42.0f;
        constexpr float flatTapAmplitudeScale = 7.0f;

        // Adds a sparse, deterministically-timed train of discrete
        // early-reflection taps into `data` (length `lengthSamples`, at
        // `sampleRate`), scaled by `gain` (the early side of the Early/Late
        // Balance crossfade). Tap *timing* is deterministic (a function of
        // Space/Size/tap index only) so that measured density in successive
        // time sub-windows is provably non-decreasing across the buildup
        // phase (see tests/ImpulseResponseGeneratorTests.cpp's density
        // test) - only per-tap amplitude/polarity jitter uses `random`, for
        // a less mechanical pattern than perfectly uniform taps. Tap 0 is
        // always placed at sample 0 with fixed (non-jittered, positive)
        // amplitude - a proxy for the earliest/loudest reflection off the
        // nearest boundary - so the generated IR's onset never depends on
        // Space/Size/Balance (Pre-Delay timing relies on measuring that
        // onset).
        void addEarlyReflections (float* data, int lengthSamples, double sampleRate,
                                   SpaceType space, float size01, float gain, juce::Random& random)
        {
            if (gain <= 0.0f || lengthSamples <= 0)
                return;

            data[0] += gain;

            const auto characteristics = characteristicsFor (space);
            const auto clampedSize = juce::jlimit (minSize01, maxSize01, size01);

            auto buildupMs = juce::jmap (clampedSize, 0.0f, 1.0f,
                                          characteristics.buildupMsAtSizeZero, characteristics.buildupMsAtSizeOne);
            auto flatEndMs = juce::jmap (clampedSize, 0.0f, 1.0f,
                                          characteristics.flatEndMsAtSizeZero, characteristics.flatEndMsAtSizeOne);

            // Short-Decay defensive scaling (see the header's doc comment
            // and the dedicated test): if the requested Decay makes the
            // buffer itself shorter than the early-reflection window, scale
            // the window down proportionally rather than letting tap
            // positions collapse onto the buffer's last sample.
            const auto flatEndSamplesRaw = juce::jmax (1, static_cast<int> (std::round (flatEndMs * 0.001 * sampleRate)));

            if (flatEndSamplesRaw > lengthSamples)
            {
                const auto scale = static_cast<float> (lengthSamples) / static_cast<float> (flatEndSamplesRaw);
                buildupMs *= scale;
                flatEndMs *= scale;
            }

            const auto numBuildupTaps = juce::jmax (1, static_cast<int> (std::round (static_cast<float> (characteristics.numTaps) * buildupTapFraction)));
            const auto numFlatTaps = juce::jmax (0, characteristics.numTaps - numBuildupTaps);

            auto placeTap = [&] (float timeMs, float amplitudeScale)
            {
                const auto sampleIndex = juce::jlimit (0, lengthSamples - 1,
                                                        static_cast<int> (std::round (timeMs * 0.001 * sampleRate)));

                const auto polarity = random.nextFloat() < 0.5f ? -1.0f : 1.0f;
                // +/-15% amplitude jitter per tap, for a less mechanical
                // reflection pattern than perfectly uniform taps.
                const auto jitter = 0.85f + 0.3f * random.nextFloat();

                data[sampleIndex] += gain * amplitudeScale * polarity * jitter;
            };

            // Density-buildup phase: the tap budget is allocated across
            // deterministic ~10ms steps with a *strictly non-decreasing*
            // tap count per step (step i, 0-indexed, is weighted i+1 - a
            // triangular allocation, more taps in later/denser steps),
            // largest-remainder-rounded so the total matches
            // numBuildupTaps exactly while never breaking the non-
            // decreasing property. This is what makes measured tap density
            // in successive 10ms sub-windows *provably* non-decreasing by
            // construction (see tests/ImpulseResponseGeneratorTests.cpp's
            // density test) - rather than emergent from a continuous warp
            // curve, which is fragile against arbitrary bin boundaries at
            // the tap counts used here. Taps are placed at deterministic,
            // evenly-spaced sub-times within their step (never random) so
            // consecutive taps get closer together (denser) as time
            // approaches buildupMs, replacing v1's uniformly-random
            // placement and geometrically-decaying amplitude.
            constexpr float densityStepMs = 10.0f;
            const auto numSteps = juce::jmax (1, static_cast<int> (std::ceil (buildupMs / densityStepMs)));

            std::vector<int> tapsPerStep (static_cast<size_t> (numSteps), 0);
            {
                const auto weightSum = static_cast<float> (numSteps * (numSteps + 1) / 2); // 1 + 2 + ... + numSteps
                int allocated = 0;

                for (int step = 0; step < numSteps; ++step)
                {
                    const auto weight = static_cast<float> (step + 1);
                    const auto count = static_cast<int> (std::floor (static_cast<float> (numBuildupTaps) * weight / weightSum));
                    tapsPerStep[static_cast<size_t> (step)] = count;
                    allocated += count;
                }

                // Distribute any rounding shortfall to the *last* steps
                // first, which can only strengthen (never break) the
                // non-decreasing property established above.
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
                    const auto timeMs = stepStartMs + u * (stepEndMs - stepStartMs);
                    placeTap (timeMs, buildupTapAmplitudeScale);
                }
            }

            // Flat-density handoff phase: evenly spaced (constant density)
            // taps at a lower, roughly constant amplitude plateau, matching
            // Griesinger's "holds roughly flat energy through ~160ms"
            // finding, before the diffuse late tail takes over.
            for (int tap = 1; tap <= numFlatTaps; ++tap)
            {
                const auto u = static_cast<float> (tap) / static_cast<float> (numFlatTaps + 1);
                const auto timeMs = buildupMs + u * (flatEndMs - buildupMs);
                placeTap (timeMs, flatTapAmplitudeScale);
            }
        }
    }

    juce::AudioBuffer<float> generateProceduralImpulseResponse (double sampleRate,
                                                                  float decaySeconds,
                                                                  float dampingHz,
                                                                  int numChannels,
                                                                  SpaceType space,
                                                                  float earlyLateBalance01,
                                                                  bool freeze,
                                                                  int seed,
                                                                  float size01,
                                                                  float bassDecayMultiplier)
    {
        const auto clampedDecay = juce::jlimit (minDecaySeconds, maxDecaySeconds, decaySeconds);

        // Keep the damping cutoff comfortably below Nyquist regardless of
        // sample rate, mirroring the defensive clamping pattern used for
        // filter cutoffs elsewhere in the suite (see e.g. Overture's
        // clampBelowNyquist) - a cutoff at or above Nyquist would produce a
        // degenerate/unstable one-pole coefficient below.
        const auto nyquist = static_cast<float> (sampleRate * 0.5);
        const auto clampedDamping = juce::jlimit (minDampingHz, juce::jmin (maxDampingHz, nyquist * 0.9f), dampingHz);
        const auto clampedBalance = juce::jlimit (0.0f, 1.0f, earlyLateBalance01);
        const auto clampedBassDecay = juce::jlimit (minBassDecayMultiplier, maxBassDecayMultiplier, bassDecayMultiplier);

        // Multiband crossovers (research-derived, see the header), re-
        // clamped below Nyquist for low sample rates.
        const auto highCrossoverHz = juce::jmin (midHighCrossoverHz, nyquist * 0.9f);
        const auto lowCrossoverHz = juce::jmin (lowMidCrossoverHz, highCrossoverHz * 0.5f);

        // The high band's progressively descending cutoff (see the
        // header's Damping doc comment) starts brighter than the terminal
        // Damping corner and descends to it monotonically over the tail's
        // length; clamped so it can never *exceed* Nyquist or sit *below*
        // clampedDamping (a no-descent degenerate case at extreme
        // Damping/sample-rate combinations, never an ascent).
        const auto brightStartHz = juce::jmax (clampedDamping, juce::jmin (nyquist * 0.9f, clampedDamping * 2.5f));

        const auto lengthSamples = juce::jmax (1, static_cast<int> (std::round (clampedDecay * sampleRate)));
        const auto channels = juce::jlimit (1, 2, numChannels);

        juce::AudioBuffer<float> impulseResponse (channels, lengthSamples);

        // -60 dB (RT60) point at t == clampedDecay: ln(1000) = 6.907755...
        constexpr float negSixtyDbTimeConstant = 6.90775528f;
        const auto decayRateMid = negSixtyDbTimeConstant / clampedDecay;
        // BassDecay (25-175%) scales the low band's RT60 relative to the
        // mid band's; the high band gets an implicit (non-parameterized)
        // ~80% RT60 multiplier - see the header's doc comments.
        const auto decayRateLow = decayRateMid / clampedBassDecay;
        const auto decayRateHigh = decayRateMid / highBandDecayMultiplier;

        const auto lowCoeff = onePoleCoefficient (lowCrossoverHz, sampleRate);
        const auto midCoeff = onePoleCoefficient (highCrossoverHz, sampleRate);
        // Freeze uses a static terminal-Damping cutoff (no time-varying
        // descent) so a sustained texture holds one consistent spectral
        // color rather than continuing to darken while frozen - see the
        // header's Freeze doc comment.
        const auto frozenHighCoeff = onePoleCoefficient (clampedDamping, sampleRate);

        // Equal-power crossfade between the early-reflection layer (full
        // gain at balance == 0) and the diffuse late tail (full gain at
        // balance == 1), so sweeping Early/Late Balance has no dip in total
        // energy at intermediate settings. Freeze suppresses the early
        // layer entirely (see the header's Freeze doc comment) and forces
        // every band's envelope flat further down.
        const auto halfPi = juce::MathConstants<float>::halfPi;
        const auto lateGain = freeze ? 1.0f : std::sin (clampedBalance * halfPi);
        const auto earlyGain = freeze ? 0.0f : std::cos (clampedBalance * halfPi);

        for (int channel = 0; channel < channels; ++channel)
        {
            // Distinct, deterministic noise stream per channel and per
            // caller-supplied seed: decorrelated L/R tails are what gives a
            // procedurally generated IR any sense of stereo width, and
            // determinism (rather than juce::Random::getSystemRandom())
            // keeps this function's output reproducible for unit tests.
            juce::Random random (static_cast<juce::int64> (seed) * 104729 + static_cast<juce::int64> (channel) * 7919 + 17);

            auto* data = impulseResponse.getWritePointer (channel);

            float lowState = 0.0f;
            float midState = 0.0f;
            float highDarkenState = 0.0f;

            for (int i = 0; i < lengthSamples; ++i)
            {
                const auto whiteNoise = random.nextFloat() * 2.0f - 1.0f;

                // Perfect-reconstruction 3-band split: low + (mid - low) +
                // (raw - mid) == raw, before any per-band envelope/
                // darkening below - i.e. the band split alone never
                // colours the signal, only the per-band envelopes and the
                // high band's extra darkening filter do.
                lowState = (1.0f - lowCoeff) * whiteNoise + lowCoeff * lowState;
                midState = (1.0f - midCoeff) * whiteNoise + midCoeff * midState;

                const auto lowBand = lowState;
                const auto midBand = midState - lowState;
                const auto highBandRaw = whiteNoise - midState;

                // Only the high band gets the progressively descending
                // cutoff (see docs/design-brief.md's Damping section) -
                // applying it to the whole recombined signal would also
                // shorten the mid/low bands' *measured* RT60 beyond their
                // intended per-band decay rates.
                const auto t = static_cast<float> (static_cast<double> (i) / sampleRate);

                float highDarkenCoeff;

                if (freeze)
                {
                    highDarkenCoeff = frozenHighCoeff;
                }
                else
                {
                    const auto darkenProgress = juce::jlimit (0.0f, 1.0f, t / clampedDecay);
                    const auto currentHighCutoffHz = juce::jmap (darkenProgress, 0.0f, 1.0f, brightStartHz, clampedDamping);
                    highDarkenCoeff = onePoleCoefficient (currentHighCutoffHz, sampleRate);
                }

                highDarkenState = (1.0f - highDarkenCoeff) * highBandRaw + highDarkenCoeff * highDarkenState;
                const auto highBand = highDarkenState;

                const auto envLow = freeze ? 1.0f : std::exp (-t * decayRateLow);
                const auto envMid = freeze ? 1.0f : std::exp (-t * decayRateMid);
                const auto envHigh = freeze ? 1.0f : std::exp (-t * decayRateHigh);

                data[i] = (lowBand * envLow + midBand * envMid + highBand * envHigh) * lateGain;
            }

            // Independent deterministic stream from the diffuse-tail noise
            // above (different multiplier/offset), so the early-reflection
            // pattern doesn't correlate with it while staying reproducible.
            juce::Random earlyRandom (static_cast<juce::int64> (seed) * 40503 + static_cast<juce::int64> (channel) * 6151 + 101);
            addEarlyReflections (data, lengthSamples, sampleRate, space, size01, earlyGain, earlyRandom);
        }

        return impulseResponse;
    }
}
