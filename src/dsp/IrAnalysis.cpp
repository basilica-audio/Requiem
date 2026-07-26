#include "IrAnalysis.h"

#include "ImpulseResponseGenerator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace IrAnalysis
{
    namespace
    {
        // Expected fraction of Gaussian-noise samples lying outside one
        // standard deviation, erfc(1/sqrt(2)) = 0.317310... Normalising the
        // raw count by this makes NED exactly 1 for Gaussian noise, which is
        // the whole point of Abel & Huang's measure.
        constexpr float gaussianOutlierFraction = 0.3173105f;

        constexpr float nedWindowSeconds = 0.025f;
        constexpr float nedHopSeconds = 0.005f;

        // One-octave bandpass Q: BW = f0 * (2^0.5 - 2^-0.5) => Q = 1/0.7071.
        constexpr float octaveQ = 1.41421356f;

        // Bands are only measurable while their upper edge stays clear of
        // Nyquist; one octave above centre is f0 * sqrt(2).
        constexpr float bandUpperEdgeFactor = 1.41421356f;
    }

    std::array<float, numOctaveBands> octaveCentreFrequencies() noexcept
    {
        std::array<float, numOctaveBands> centres {};
        auto f = 31.25f;

        for (int band = 0; band < numOctaveBands; ++band)
        {
            centres[static_cast<size_t> (band)] = f;
            f *= 2.0f;
        }

        return centres;
    }

    bool isBandUsable (float centreHz, double sampleRate) noexcept
    {
        const auto nyquist = static_cast<float> (sampleRate * 0.5);
        return centreHz * bandUpperEdgeFactor < nyquist * 0.9f;
    }

    void filterOctaveBand (const float* input, float* output, int numSamples,
                            float centreHz, double sampleRate)
    {
        if (numSamples <= 0)
            return;

        const auto nyquist = static_cast<float> (sampleRate * 0.5);
        const auto clampedCentre = juce::jlimit (1.0f, nyquist * 0.45f, centreHz);

        auto coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (sampleRate, clampedCentre, octaveQ);

        // Two identical biquads in cascade: a 4th-order octave band, steep
        // enough that adjacent bands do not smear each other's decay rates
        // into the regression below.
        juce::dsp::IIR::Filter<float> stageOne (coefficients);
        juce::dsp::IIR::Filter<float> stageTwo (coefficients);

        juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32> (numSamples), 1 };
        stageOne.prepare (spec);
        stageTwo.prepare (spec);
        stageOne.reset();
        stageTwo.reset();

        for (int i = 0; i < numSamples; ++i)
            output[i] = stageTwo.processSample (stageOne.processSample (input[i]));
    }

    float normalisedEchoDensity (const float* data, int numSamples)
    {
        if (numSamples <= 1)
            return 0.0f;

        // Hann weighting, normalised to unit sum, exactly as in Abel &
        // Huang: the weighted standard deviation and the weighted outlier
        // count must use the same window.
        std::vector<float> weights (static_cast<size_t> (numSamples));
        double weightSum = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                     * static_cast<float> (i) / static_cast<float> (numSamples - 1));
            weights[static_cast<size_t> (i)] = w;
            weightSum += w;
        }

        if (weightSum <= 0.0)
            return 0.0f;

        for (auto& w : weights)
            w = static_cast<float> (w / weightSum);

        double variance = 0.0;

        for (int i = 0; i < numSamples; ++i)
            variance += static_cast<double> (weights[static_cast<size_t> (i)])
                         * static_cast<double> (data[i]) * static_cast<double> (data[i]);

        const auto standardDeviation = static_cast<float> (std::sqrt (variance));

        if (standardDeviation <= 0.0f)
            return 0.0f;

        double outliers = 0.0;

        for (int i = 0; i < numSamples; ++i)
            if (std::abs (data[i]) > standardDeviation)
                outliers += static_cast<double> (weights[static_cast<size_t> (i)]);

        return static_cast<float> (outliers / gaussianOutlierFraction);
    }

    float estimateMixingTimeSeconds (const float* data, int numSamples, double sampleRate,
                                      std::vector<float>* nedCurveOut)
    {
        const auto windowSamples = juce::jmax (8, static_cast<int> (std::round (nedWindowSeconds * sampleRate)));
        const auto hopSamples = juce::jmax (1, static_cast<int> (std::round (nedHopSeconds * sampleRate)));

        if (nedCurveOut != nullptr)
            nedCurveOut->clear();

        auto mixingTime = maxMixingTimeSeconds;
        auto found = false;

        for (int start = 0; start + windowSamples <= numSamples; start += hopSamples)
        {
            const auto ned = normalisedEchoDensity (data + start, windowSamples);

            if (nedCurveOut != nullptr)
                nedCurveOut->push_back (ned);

            if (! found && ned >= diffuseNedThreshold)
            {
                // The window is centred on its own midpoint, so the instant
                // the field became diffuse is the window's centre, not its
                // start - reporting the start would bias t_mix low by half a
                // window (12.5 ms) on every IR.
                mixingTime = static_cast<float> ((start + windowSamples / 2) / sampleRate);
                found = true;

                if (nedCurveOut == nullptr)
                    break;
            }
        }

        return juce::jlimit (minMixingTimeSeconds, maxMixingTimeSeconds, mixingTime);
    }

    float schroederRt60Seconds (const float* bandData, int numSamples, double sampleRate,
                                 float* rSquaredOut)
    {
        if (rSquaredOut != nullptr)
            *rSquaredOut = 0.0f;

        if (numSamples < 16)
            return 0.0f;

        // Schroeder backward integration: EDC(t) = integral from t to
        // infinity of h^2, which is far smoother than the squared impulse
        // response itself and is what makes a straight-line fit meaningful.
        std::vector<double> energyDecayCurve (static_cast<size_t> (numSamples));
        double runningSum = 0.0;

        for (int i = numSamples - 1; i >= 0; --i)
        {
            runningSum += static_cast<double> (bandData[i]) * static_cast<double> (bandData[i]);
            energyDecayCurve[static_cast<size_t> (i)] = runningSum;
        }

        const auto total = energyDecayCurve[0];

        if (total <= 0.0)
            return 0.0f;

        // ISO 3382 T30: regress the -5 dB to -35 dB span and extrapolate to
        // -60 dB. Starting at -5 dB skips the direct-sound/early-reflection
        // transient, which is not part of the diffuse decay.
        constexpr double startDb = -5.0;
        constexpr double endDb = -35.0;

        auto startIndex = -1;
        auto endIndex = -1;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto level = 10.0 * std::log10 (juce::jmax (1.0e-30, energyDecayCurve[static_cast<size_t> (i)] / total));

            if (startIndex < 0 && level <= startDb)
                startIndex = i;

            if (level <= endDb)
            {
                endIndex = i;
                break;
            }
        }

        if (startIndex < 0 || endIndex < 0 || endIndex - startIndex < 8)
            return 0.0f;

        // Ordinary least squares on (t, dB).
        const auto count = endIndex - startIndex;
        double sumT = 0.0, sumL = 0.0, sumTT = 0.0, sumTL = 0.0, sumLL = 0.0;

        for (int i = startIndex; i < endIndex; ++i)
        {
            const auto t = static_cast<double> (i) / sampleRate;
            const auto level = 10.0 * std::log10 (juce::jmax (1.0e-30, energyDecayCurve[static_cast<size_t> (i)] / total));

            sumT += t;
            sumL += level;
            sumTT += t * t;
            sumTL += t * level;
            sumLL += level * level;
        }

        const auto n = static_cast<double> (count);
        const auto denominator = n * sumTT - sumT * sumT;

        if (std::abs (denominator) < 1.0e-20)
            return 0.0f;

        const auto slope = (n * sumTL - sumT * sumL) / denominator;

        if (slope >= 0.0)
            return 0.0f;

        if (rSquaredOut != nullptr)
        {
            const auto varianceL = n * sumLL - sumL * sumL;
            const auto covariance = n * sumTL - sumT * sumL;
            const auto rSquared = varianceL > 0.0 ? (covariance * covariance) / (denominator * varianceL) : 0.0;
            *rSquaredOut = static_cast<float> (juce::jlimit (0.0, 1.0, rSquared));
        }

        return static_cast<float> (-60.0 / slope);
    }

    std::array<float, numOctaveBands> octaveBandEnergies (const float* data, int numSamples,
                                                            int startSample, double sampleRate)
    {
        std::array<float, numOctaveBands> energies {};
        const auto centres = octaveCentreFrequencies();

        if (numSamples <= 0)
            return energies;

        std::vector<float> banded (static_cast<size_t> (numSamples));

        for (int band = 0; band < numOctaveBands; ++band)
        {
            const auto centre = centres[static_cast<size_t> (band)];

            if (! isBandUsable (centre, sampleRate))
                continue;

            filterOctaveBand (data, banded.data(), numSamples, centre, sampleRate);

            double sum = 0.0;

            for (int i = juce::jmax (0, startSample); i < numSamples; ++i)
                sum += static_cast<double> (banded[static_cast<size_t> (i)]) * static_cast<double> (banded[static_cast<size_t> (i)]);

            energies[static_cast<size_t> (band)] = static_cast<float> (sum);
        }

        return energies;
    }

    namespace
    {
        // Fills invalid/failed bands from their nearest valid neighbour so
        // downstream consumers never see a zero RT60 (which would ask the
        // attenuation designer for infinite attenuation).
        void backfillInvalidBands (std::array<float, numOctaveBands>& values,
                                    const std::array<bool, numOctaveBands>& valid,
                                    float fallback)
        {
            auto anyValid = false;

            for (auto v : valid)
                anyValid = anyValid || v;

            if (! anyValid)
            {
                values.fill (fallback);
                return;
            }

            for (int band = 0; band < numOctaveBands; ++band)
            {
                if (valid[static_cast<size_t> (band)])
                    continue;

                auto nearest = -1;
                auto nearestDistance = numOctaveBands + 1;

                for (int other = 0; other < numOctaveBands; ++other)
                {
                    if (! valid[static_cast<size_t> (other)])
                        continue;

                    const auto distance = std::abs (other - band);

                    if (distance < nearestDistance)
                    {
                        nearestDistance = distance;
                        nearest = other;
                    }
                }

                values[static_cast<size_t> (band)] = values[static_cast<size_t> (nearest)];
            }
        }

        // Mono sum of an IR, which is what both the NED estimate and the
        // per-band decay fit operate on: t_mix and RT60 are properties of the
        // room, not of the capture's stereo image.
        std::vector<float> monoSum (const juce::AudioBuffer<float>& buffer)
        {
            const auto numSamples = buffer.getNumSamples();
            const auto numChannels = juce::jmax (1, buffer.getNumChannels());

            std::vector<float> mono (static_cast<size_t> (juce::jmax (0, numSamples)), 0.0f);

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                const auto* source = buffer.getReadPointer (channel);

                for (int i = 0; i < numSamples; ++i)
                    mono[static_cast<size_t> (i)] += source[i];
            }

            const auto scale = 1.0f / static_cast<float> (numChannels);

            for (auto& sample : mono)
                sample *= scale;

            return mono;
        }

        Analysis analyseCommon (const juce::AudioBuffer<float>& impulseResponse, double sampleRate,
                                 bool measureRt60)
        {
            Analysis analysis;
            analysis.bandValid.fill (false);
            analysis.rt60Octave.fill (0.0f);
            analysis.edrAtMixingTime.fill (0.0f);
            analysis.regressionRSquared.fill (0.0f);

            const auto numSamples = impulseResponse.getNumSamples();

            if (numSamples <= 0 || sampleRate <= 0.0)
                return analysis;

            const auto mono = monoSum (impulseResponse);

            analysis.mixingTimeSeconds = estimateMixingTimeSeconds (mono.data(), numSamples, sampleRate);

            const auto mixingSample = juce::jlimit (0, juce::jmax (0, numSamples - 1),
                                                     static_cast<int> (std::round (analysis.mixingTimeSeconds * sampleRate)));

            const auto centres = octaveCentreFrequencies();
            std::vector<float> banded (static_cast<size_t> (numSamples));
            auto poorlyFittedBands = 0;

            for (int band = 0; band < numOctaveBands; ++band)
            {
                const auto centre = centres[static_cast<size_t> (band)];

                if (! isBandUsable (centre, sampleRate))
                    continue;

                filterOctaveBand (mono.data(), banded.data(), numSamples, centre, sampleRate);

                double residualEnergy = 0.0;

                for (int i = mixingSample; i < numSamples; ++i)
                    residualEnergy += static_cast<double> (banded[static_cast<size_t> (i)])
                                       * static_cast<double> (banded[static_cast<size_t> (i)]);

                analysis.edrAtMixingTime[static_cast<size_t> (band)] = static_cast<float> (residualEnergy);

                if (! measureRt60)
                {
                    analysis.bandValid[static_cast<size_t> (band)] = true;
                    continue;
                }

                float rSquared = 0.0f;
                const auto rt60 = schroederRt60Seconds (banded.data(), numSamples, sampleRate, &rSquared);

                analysis.regressionRSquared[static_cast<size_t> (band)] = rSquared;

                if (rt60 > 0.0f)
                {
                    analysis.rt60Octave[static_cast<size_t> (band)] = rt60;
                    analysis.bandValid[static_cast<size_t> (band)] = true;

                    if (rSquared < minRegressionRSquared)
                        ++poorlyFittedBands;
                }
                else
                {
                    ++poorlyFittedBands;
                }
            }

            if (measureRt60)
            {
                backfillInvalidBands (analysis.rt60Octave, analysis.bandValid, 1.0f);
                analysis.hasLowConfidence = poorlyFittedBands >= lowConfidenceBandCount;
            }

            return analysis;
        }
    }

    Analysis analyse (const juce::AudioBuffer<float>& impulseResponse, double sampleRate)
    {
        return analyseCommon (impulseResponse, sampleRate, true);
    }

    Analysis analyseProcedural (const juce::AudioBuffer<float>& impulseResponse, double sampleRate,
                                 float decaySeconds, float dampingHz, float bassDecayMultiplier)
    {
        // NED/EDR still measured against the real buffer; only the RT60 fit
        // is replaced by the generator's own analytic per-band decay law
        // (see ImpulseResponseGenerator.h): mid = Decay, low band (below the
        // ~500 Hz crossover) = Decay * BassDecay, high band (above the ~5 kHz
        // crossover) = Decay * highBandDecayMultiplier, with the terminal HF
        // corner at Damping pulling the topmost bands down further.
        auto analysis = analyseCommon (impulseResponse, sampleRate, false);

        const auto centres = octaveCentreFrequencies();

        for (int band = 0; band < numOctaveBands; ++band)
        {
            const auto centre = centres[static_cast<size_t> (band)];
            const auto rt60 = ReverbIR::analyticRt60Seconds (centre, decaySeconds, dampingHz, bassDecayMultiplier);

            analysis.rt60Octave[static_cast<size_t> (band)] = juce::jmax (0.05f, rt60);
            analysis.bandValid[static_cast<size_t> (band)] = isBandUsable (centre, sampleRate);
            analysis.regressionRSquared[static_cast<size_t> (band)] = 1.0f;
        }

        analysis.hasLowConfidence = false;
        return analysis;
    }

    std::array<float, correctionFirLength> designCorrectionFir (const std::array<float, numOctaveBands>& perBandGains,
                                                                  double sampleRate)
    {
        std::array<float, correctionFirLength> taps {};
        taps.fill (0.0f);

        const auto centres = octaveCentreFrequencies();

        // Target magnitude on the FFT's own frequency grid, log-interpolated
        // between octave centres and then 1/3-octave smoothed (brief 3.2 /
        // Carpentier eq. 3) so the correction never introduces a step at a
        // band boundary that the 256-tap window could not realise anyway.
        constexpr int fftOrder = 8; // 2^8 == correctionFirLength
        static_assert (1 << fftOrder == correctionFirLength, "FFT order must match the FIR length");

        const auto numBins = correctionFirLength / 2 + 1;
        std::vector<float> magnitude (static_cast<size_t> (numBins), 1.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto frequency = static_cast<float> (bin * sampleRate / correctionFirLength);

            if (frequency <= centres.front())
            {
                magnitude[static_cast<size_t> (bin)] = perBandGains.front();
                continue;
            }

            if (frequency >= centres.back())
            {
                magnitude[static_cast<size_t> (bin)] = perBandGains.back();
                continue;
            }

            auto upper = 1;

            while (upper < numOctaveBands - 1 && centres[static_cast<size_t> (upper)] < frequency)
                ++upper;

            const auto lower = upper - 1;
            const auto fLow = centres[static_cast<size_t> (lower)];
            const auto fHigh = centres[static_cast<size_t> (upper)];
            const auto t = std::log (frequency / fLow) / std::log (fHigh / fLow);

            // Interpolate in dB, not in linear gain: a spectral tilt is a
            // straight line on a log-log plot, not on a log-linear one.
            const auto dbLow = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, perBandGains[static_cast<size_t> (lower)]));
            const auto dbHigh = juce::Decibels::gainToDecibels (juce::jmax (1.0e-6f, perBandGains[static_cast<size_t> (upper)]));

            magnitude[static_cast<size_t> (bin)] = juce::Decibels::decibelsToGain (dbLow + static_cast<float> (t) * (dbHigh - dbLow));
        }

        // 1/3-octave smoothing pass over the magnitude grid.
        {
            std::vector<float> smoothed (magnitude);

            for (int bin = 1; bin < numBins; ++bin)
            {
                const auto frequency = static_cast<float> (bin * sampleRate / correctionFirLength);
                const auto lowEdge = frequency / 1.122462f;  // 2^(1/6)
                const auto highEdge = frequency * 1.122462f;

                const auto firstBin = juce::jmax (0, static_cast<int> (std::floor (lowEdge * correctionFirLength / sampleRate)));
                const auto lastBin = juce::jmin (numBins - 1, static_cast<int> (std::ceil (highEdge * correctionFirLength / sampleRate)));

                double sum = 0.0;
                auto count = 0;

                for (int k = firstBin; k <= lastBin; ++k)
                {
                    sum += magnitude[static_cast<size_t> (k)];
                    ++count;
                }

                if (count > 0)
                    smoothed[static_cast<size_t> (bin)] = static_cast<float> (sum / count);
            }

            magnitude.swap (smoothed);
        }

        // Zero-phase inverse FFT, then circular-shift by half the length and
        // Hann-window: the classic frequency-sampling design for a
        // linear-phase FIR whose group delay is exactly correctionFirGroupDelay.
        juce::dsp::FFT fft (fftOrder);
        std::vector<float> fftData (static_cast<size_t> (2 * correctionFirLength), 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            fftData[static_cast<size_t> (2 * bin)] = magnitude[static_cast<size_t> (bin)];

            if (bin > 0 && bin < correctionFirLength / 2)
                fftData[static_cast<size_t> (2 * (correctionFirLength - bin))] = magnitude[static_cast<size_t> (bin)];
        }

        fft.performRealOnlyInverseTransform (fftData.data());

        for (int n = 0; n < correctionFirLength; ++n)
        {
            const auto shifted = (n + correctionFirGroupDelay) % correctionFirLength;
            const auto window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                          * static_cast<float> (n) / static_cast<float> (correctionFirLength - 1));
            taps[static_cast<size_t> (n)] = fftData[static_cast<size_t> (shifted)] * window;
        }

        return taps;
    }

    float spliceWindow (int sampleIndex, double sampleRate, float mixingTimeSeconds) noexcept
    {
        const auto fadeSamples = juce::jmax (1, static_cast<int> (std::round (spliceFadeSeconds * sampleRate)));
        const auto fadeStart = juce::jmax (0, static_cast<int> (std::round (mixingTimeSeconds * sampleRate)) - fadeSamples / 2);
        const auto fadeEnd = fadeStart + fadeSamples;

        if (sampleIndex <= fadeStart)
            return 1.0f;

        if (sampleIndex >= fadeEnd)
            return 0.0f;

        const auto t = static_cast<float> (sampleIndex - fadeStart) / static_cast<float> (fadeSamples);
        // Raised cosine: smooth in value and in first derivative at both
        // ends, so h * w has no discontinuity the FFT convolution would ring on.
        return 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * t);
    }
}
