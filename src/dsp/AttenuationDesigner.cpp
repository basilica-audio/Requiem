#include "AttenuationDesigner.h"

#include <algorithm>
#include <cmath>

namespace AttenuationDesign
{
    namespace
    {
        // Reference command gain the interaction matrix is measured at.
        // Small enough that the sections' dB responses are still close to
        // linear in their command gain (which is what makes B a good fixed
        // Jacobian), large enough not to be swamped by float rounding.
        constexpr float interactionReferenceDb = 1.0f;

        constexpr float octaveQ = 1.41421356f;
        constexpr float shelfQ = 0.70710678f;

        // Deepest per-sample attenuation the designer will ask for. A T60
        // shorter than this against a long delay line would demand a
        // physically unrealisable per-traversal attenuation.
        constexpr float minT60Seconds = 0.05f;

        void biquadFromCoefficients (Biquad& destination,
                                      const juce::dsp::IIR::Coefficients<float>::Ptr& coefficients) noexcept
        {
            // juce::dsp::IIR::Coefficients stores [b0, b1, b2, a1, a2]
            // already normalised by a0.
            const auto* raw = coefficients->getRawCoefficients();
            destination.b0 = raw[0];
            destination.b1 = raw[1];
            destination.b2 = raw[2];
            destination.a1 = raw[3];
            destination.a2 = raw[4];
        }

        //======================================================================
        // Householder QR least squares. B is `rows` x `cols`, row-major.
        // Produces the pseudo-inverse (cols x rows, row-major) so each later
        // solve is a single matrix-vector product.
        std::vector<float> pseudoInverseByQr (std::vector<double> b, int rows, int cols)
        {
            std::vector<double> q (static_cast<size_t> (rows * rows), 0.0);

            for (int i = 0; i < rows; ++i)
                q[static_cast<size_t> (i * rows + i)] = 1.0;

            for (int k = 0; k < cols; ++k)
            {
                double norm = 0.0;

                for (int i = k; i < rows; ++i)
                    norm += b[static_cast<size_t> (i * cols + k)] * b[static_cast<size_t> (i * cols + k)];

                norm = std::sqrt (norm);

                if (norm < 1.0e-18)
                    continue;

                const auto alpha = b[static_cast<size_t> (k * cols + k)] > 0.0 ? -norm : norm;

                std::vector<double> v (static_cast<size_t> (rows), 0.0);

                for (int i = k; i < rows; ++i)
                    v[static_cast<size_t> (i)] = b[static_cast<size_t> (i * cols + k)];

                v[static_cast<size_t> (k)] -= alpha;

                double vNorm = 0.0;

                for (int i = k; i < rows; ++i)
                    vNorm += v[static_cast<size_t> (i)] * v[static_cast<size_t> (i)];

                if (vNorm < 1.0e-24)
                    continue;

                // B <- (I - 2 v v^T / v^T v) B
                for (int j = 0; j < cols; ++j)
                {
                    double dot = 0.0;

                    for (int i = k; i < rows; ++i)
                        dot += v[static_cast<size_t> (i)] * b[static_cast<size_t> (i * cols + j)];

                    const auto scale = 2.0 * dot / vNorm;

                    for (int i = k; i < rows; ++i)
                        b[static_cast<size_t> (i * cols + j)] -= scale * v[static_cast<size_t> (i)];
                }

                // Accumulate the same reflection into Q.
                for (int j = 0; j < rows; ++j)
                {
                    double dot = 0.0;

                    for (int i = k; i < rows; ++i)
                        dot += v[static_cast<size_t> (i)] * q[static_cast<size_t> (i * rows + j)];

                    const auto scale = 2.0 * dot / vNorm;

                    for (int i = k; i < rows; ++i)
                        q[static_cast<size_t> (i * rows + j)] -= scale * v[static_cast<size_t> (i)];
                }
            }

            // Q currently holds the product of the reflections applied to I,
            // i.e. Q^T of the usual factorisation, so its first `cols` rows
            // are what multiplies the residual. Back-substitute R x = (Q^T r)
            // column by column against the identity to get the pseudo-inverse.
            std::vector<float> pinv (static_cast<size_t> (cols * rows), 0.0f);

            for (int column = 0; column < rows; ++column)
            {
                std::vector<double> rhs (static_cast<size_t> (cols), 0.0);

                for (int i = 0; i < cols; ++i)
                    rhs[static_cast<size_t> (i)] = q[static_cast<size_t> (i * rows + column)];

                for (int i = cols - 1; i >= 0; --i)
                {
                    auto sum = rhs[static_cast<size_t> (i)];

                    for (int j = i + 1; j < cols; ++j)
                        sum -= b[static_cast<size_t> (i * cols + j)] * rhs[static_cast<size_t> (j)];

                    const auto diagonal = b[static_cast<size_t> (i * cols + i)];
                    rhs[static_cast<size_t> (i)] = std::abs (diagonal) > 1.0e-18 ? sum / diagonal : 0.0;
                }

                for (int i = 0; i < cols; ++i)
                    pinv[static_cast<size_t> (i * rows + column)] = static_cast<float> (rhs[static_cast<size_t> (i)]);
            }

            return pinv;
        }
    }

    //==========================================================================
    FrequencyPoint FrequencyPoint::fromHertz (float frequencyHz, double sampleRate) noexcept
    {
        const auto omega = juce::MathConstants<float>::twoPi * frequencyHz / static_cast<float> (sampleRate);

        FrequencyPoint point;
        point.cosOmega = std::cos (omega);
        point.cosTwoOmega = std::cos (2.0f * omega);
        point.sinOmega = std::sin (omega);
        point.sinTwoOmega = std::sin (2.0f * omega);
        return point;
    }

    float Biquad::magnitudeDb (const FrequencyPoint& point) const noexcept
    {
        const auto cosW = point.cosOmega;
        const auto cos2W = point.cosTwoOmega;
        const auto sinW = point.sinOmega;
        const auto sin2W = point.sinTwoOmega;

        const auto numeratorReal = b0 + b1 * cosW + b2 * cos2W;
        const auto numeratorImag = -(b1 * sinW + b2 * sin2W);
        const auto denominatorReal = 1.0f + a1 * cosW + a2 * cos2W;
        const auto denominatorImag = -(a1 * sinW + a2 * sin2W);

        const auto numeratorMagSq = numeratorReal * numeratorReal + numeratorImag * numeratorImag;
        const auto denominatorMagSq = denominatorReal * denominatorReal + denominatorImag * denominatorImag;

        if (denominatorMagSq <= 0.0f)
            return 0.0f;

        return 10.0f * std::log10 (juce::jmax (1.0e-20f, numeratorMagSq / denominatorMagSq));
    }

    float perSampleAttenuationDb (float t60Seconds, double sampleRate) noexcept
    {
        const auto clamped = juce::jmax (minT60Seconds, t60Seconds);
        return static_cast<float> (-60.0 / (sampleRate * static_cast<double> (clamped)));
    }

    //==========================================================================
    void DesignContext::prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        bandCentres = IrAnalysis::octaveCentreFrequencies();

        // The designer's own usability criteria, deliberately *not*
        // IrAnalysis::isBandUsable(): that one asks "can this octave band be
        // measured through a bandpass", which is the wrong question here. A
        // section is usable when its own design frequency - a peak's centre
        // plus its upper skirt, or a shelf's corner - stays clear of Nyquist.
        // Judging the 16 kHz *high shelf* by a bandpass criterion disables it
        // at 48 kHz, leaving everything above 11 kHz uncontrolled; the
        // least-squares fit then spreads that unreachable residual back across
        // the whole curve and every band's T60 comes out wrong.
        const auto maxSafeHz = static_cast<float> (sampleRate * 0.5) * 0.9f;

        for (int band = 0; band < numBands; ++band)
        {
            const auto centre = bandCentres[static_cast<size_t> (band)];
            const auto designFrequency = band == numBands - 1 ? centre * 0.70710678f : centre * 1.41421356f;
            bandUsable[static_cast<size_t> (band)] = designFrequency < maxSafeHz;
        }

        // Control grid: band centres interleaved with geometric midpoints.
        for (int band = 0; band < numBands; ++band)
        {
            controlFrequencies[static_cast<size_t> (2 * band)] = bandCentres[static_cast<size_t> (band)];

            if (band + 1 < numBands)
                controlFrequencies[static_cast<size_t> (2 * band + 1)] =
                    std::sqrt (bandCentres[static_cast<size_t> (band)] * bandCentres[static_cast<size_t> (band + 1)]);
        }

        for (int j = 0; j < numControlFrequencies; ++j)
        {
            controlUsable[static_cast<size_t> (j)] = controlFrequencies[static_cast<size_t> (j)] < maxSafeHz;
            controlPoints[static_cast<size_t> (j)] = FrequencyPoint::fromHertz (controlFrequencies[static_cast<size_t> (j)], sampleRate);
        }

        for (int band = 0; band < numBands; ++band)
            bandPoints[static_cast<size_t> (band)] = FrequencyPoint::fromHertz (bandCentres[static_cast<size_t> (band)], sampleRate);

        // Stability grid: DC, Nyquist, and log-spaced points in between. The
        // finished cascade's magnitude is checked here and shifted down if it
        // ever reaches unity - see design()'s stability projection.
        {
            const auto nyquistHz = static_cast<float> (sampleRate * 0.5);
            auto next = 0;

            const auto add = [&] (float frequencyHz)
            {
                if (next < stabilityGridSize)
                    stabilityPoints[static_cast<size_t> (next++)] =
                        FrequencyPoint::fromHertz (juce::jlimit (0.0f, nyquistHz, frequencyHz), sampleRate);
            };

            add (0.0f);
            add (nyquistHz);

            // Every section centre and control frequency: a cascade of shelves
            // and peaks attains its extrema at (or within a hair of) these.
            for (int band = 0; band < numBands; ++band)
                add (bandCentres[static_cast<size_t> (band)]);

            for (int j = 0; j < numControlFrequencies; ++j)
                add (controlFrequencies[static_cast<size_t> (j)]);

            // Fill the remainder with a log-spaced sweep so nothing between the
            // structural points is missed either.
            const auto remaining = stabilityGridSize - next;

            for (int i = 0; i < remaining; ++i)
            {
                const auto t = remaining > 1 ? static_cast<float> (i) / static_cast<float> (remaining - 1) : 0.0f;
                add (10.0f * std::pow (nyquistHz / 10.0f, t));
            }
        }

        // Interaction matrix: column k is the dB response of section k at
        // unit command gain, sampled at every control frequency. The last
        // column is the broadband gain, flat by definition.
        std::vector<double> interaction (static_cast<size_t> (numControlFrequencies * numCommandGains), 0.0);

        for (int k = 0; k < numBands; ++k)
        {
            if (! bandUsable[static_cast<size_t> (k)])
                continue;

            const auto section = makeSection (k, interactionReferenceDb);

            for (int j = 0; j < numControlFrequencies; ++j)
            {
                if (! controlUsable[static_cast<size_t> (j)])
                    continue;

                interaction[static_cast<size_t> (j * numCommandGains + k)] =
                    static_cast<double> (section.magnitudeDb (controlPoints[static_cast<size_t> (j)]) / interactionReferenceDb);
            }
        }

        for (int j = 0; j < numControlFrequencies; ++j)
            if (controlUsable[static_cast<size_t> (j)])
                interaction[static_cast<size_t> (j * numCommandGains + numBands)] = 1.0;

        pseudoInverse = pseudoInverseByQr (std::move (interaction), numControlFrequencies, numCommandGains);
        prepared = true;
    }

    Biquad DesignContext::makeSection (int band, float gainDb) const
    {
        Biquad section;

        const auto centre = bandCentres[static_cast<size_t> (band)];
        const auto nyquist = static_cast<float> (sampleRate * 0.5);
        const auto gain = juce::Decibels::decibelsToGain (gainDb);

        juce::dsp::IIR::Coefficients<float>::Ptr coefficients;

        if (band == 0)
        {
            // Low shelf: corner at the band's upper edge, so it owns
            // everything below the first peaking section.
            coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf (
                sampleRate, juce::jlimit (10.0f, nyquist * 0.45f, centre * 1.41421356f), shelfQ, gain);
        }
        else if (band == numBands - 1)
        {
            coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                sampleRate, juce::jlimit (10.0f, nyquist * 0.45f, centre * 0.70710678f), shelfQ, gain);
        }
        else
        {
            coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                sampleRate, juce::jlimit (10.0f, nyquist * 0.45f, centre), octaveQ, gain);
        }

        biquadFromCoefficients (section, coefficients);
        return section;
    }

    float DesignContext::cascadeMagnitudeDb (const std::array<Biquad, numBands>& sections,
                                              const FrequencyPoint& point) const
    {
        auto total = 0.0f;

        for (const auto& section : sections)
            total += section.magnitudeDb (point);

        return total;
    }

    void DesignContext::solveLeastSquares (const std::array<float, numControlFrequencies>& residual,
                                            std::array<float, numCommandGains>& deltaOut) const
    {
        for (int i = 0; i < numCommandGains; ++i)
        {
            auto sum = 0.0f;

            for (int j = 0; j < numControlFrequencies; ++j)
                sum += pseudoInverse[static_cast<size_t> (i * numControlFrequencies + j)] * residual[static_cast<size_t> (j)];

            deltaOut[static_cast<size_t> (i)] = sum;
        }
    }

    LineAttenuation DesignContext::design (const std::array<float, numBands>& t60Seconds, int delaySamples) const
    {
        LineAttenuation attenuation;

        if (! prepared || delaySamples <= 0)
            return attenuation;

        //======================================================================
        // Target: dB attenuation this line must apply per traversal at each
        // control frequency, T60 log-interpolated between the octave centres.
        std::array<float, numControlFrequencies> target {};

        for (int j = 0; j < numControlFrequencies; ++j)
        {
            if (! controlUsable[static_cast<size_t> (j)])
            {
                target[static_cast<size_t> (j)] = 0.0f;
                continue;
            }

            const auto frequency = controlFrequencies[static_cast<size_t> (j)];

            // Even control indices sit exactly on a band centre; odd ones are
            // geometric midpoints, so the T60 there is the geometric mean of
            // the two neighbouring bands' - a straight line in log-log.
            float t60;

            if (j % 2 == 0)
            {
                t60 = t60Seconds[static_cast<size_t> (j / 2)];
            }
            else
            {
                const auto lower = t60Seconds[static_cast<size_t> (j / 2)];
                const auto upper = t60Seconds[static_cast<size_t> (j / 2 + 1)];
                t60 = std::sqrt (juce::jmax (minT60Seconds, lower) * juce::jmax (minT60Seconds, upper));
            }

            juce::ignoreUnused (frequency);
            target[static_cast<size_t> (j)] = static_cast<float> (delaySamples) * perSampleAttenuationDb (t60, sampleRate);
        }

        //======================================================================
        // Decompose the target into a broadband part and a shaping part. The
        // broadband part is realised by a plain scalar gain, the shaping part
        // by the ten sections - so the +/-10 dB clamp always bites on the
        // *deviation* from the overall decay rate, never on the decay rate
        // itself. (Doing this once, up front, matters: folding the band gains'
        // running mean back into the broadband gain on every iteration is not
        // gain-neutral - the sections overlap, so removing the mean from ten
        // of them does not remove exactly that mean from the response - and it
        // fights the Gauss-Newton correction badly enough that the fit stalls
        // around a dB of error on any tilted target.)
        auto broadbandBaseDb = 0.0f;
        auto usableControlCount = 0;

        for (int j = 0; j < numControlFrequencies; ++j)
        {
            if (! controlUsable[static_cast<size_t> (j)])
                continue;

            broadbandBaseDb += target[static_cast<size_t> (j)];
            ++usableControlCount;
        }

        if (usableControlCount > 0)
            broadbandBaseDb /= static_cast<float> (usableControlCount);

        std::array<float, numControlFrequencies> shapingTarget {};

        for (int j = 0; j < numControlFrequencies; ++j)
            shapingTarget[static_cast<size_t> (j)] = controlUsable[static_cast<size_t> (j)]
                                                       ? target[static_cast<size_t> (j)] - broadbandBaseDb
                                                       : 0.0f;

        //======================================================================
        // Initialisation: a single least-squares solve against the linearised
        // interaction matrix (Schlecht & Habets step 2, gamma = R^-1 Q^T tau).
        std::array<float, numCommandGains> commandGains {};
        solveLeastSquares (shapingTarget, commandGains);

        std::array<Biquad, numBands> sections {};
        auto broadbandDb = broadbandBaseDb;

        // Working pseudo-inverse. It starts as the sample-rate-wide linearised
        // one and is refreshed below against the *current* command gains: the
        // cascade's response is a sum in dB, so d(response_j)/d(gain_k) is just
        // section k's own dB slope at control frequency j - an exact Jacobian
        // for a handful of extra section builds, no full cascade re-evaluation.
        // With the linearised matrix alone the iteration stalls around half a
        // dB on tilted targets, which at these attenuation depths is a 20%+
        // T60 error; with the exact Jacobian it converges to the least-squares
        // optimum in a few steps.
        auto workingPseudoInverse = pseudoInverse;

        const auto refreshJacobian = [&]
        {
            std::vector<double> jacobian (static_cast<size_t> (numControlFrequencies * numCommandGains), 0.0);
            constexpr float step = 0.25f;

            for (int k = 0; k < numBands; ++k)
            {
                if (! bandUsable[static_cast<size_t> (k)])
                    continue;

                const auto perturbed = makeSection (k, commandGains[static_cast<size_t> (k)] + step);
                const auto& current = sections[static_cast<size_t> (k)];

                for (int j = 0; j < numControlFrequencies; ++j)
                {
                    if (! controlUsable[static_cast<size_t> (j)])
                        continue;

                    const auto& point = controlPoints[static_cast<size_t> (j)];
                    jacobian[static_cast<size_t> (j * numCommandGains + k)] =
                        static_cast<double> ((perturbed.magnitudeDb (point) - current.magnitudeDb (point)) / step);
                }
            }

            for (int j = 0; j < numControlFrequencies; ++j)
                if (controlUsable[static_cast<size_t> (j)])
                    jacobian[static_cast<size_t> (j * numCommandGains + numBands)] = 1.0;

            workingPseudoInverse = pseudoInverseByQr (std::move (jacobian), numControlFrequencies, numCommandGains);
        };

        const auto solveWithWorkingJacobian = [&] (const std::array<float, numControlFrequencies>& residual,
                                                     std::array<float, numCommandGains>& deltaOut)
        {
            for (int i = 0; i < numCommandGains; ++i)
            {
                auto sum = 0.0f;

                for (int j = 0; j < numControlFrequencies; ++j)
                    sum += workingPseudoInverse[static_cast<size_t> (i * numControlFrequencies + j)]
                            * residual[static_cast<size_t> (j)];

                deltaOut[static_cast<size_t> (i)] = sum;
            }
        };

        const auto rebuild = [&]
        {
            for (int band = 0; band < numBands; ++band)
            {
                if (! bandUsable[static_cast<size_t> (band)])
                {
                    commandGains[static_cast<size_t> (band)] = 0.0f;
                    sections[static_cast<size_t> (band)] = Biquad {};
                    continue;
                }

                // DAFx-17's zero-instability clamp.
                commandGains[static_cast<size_t> (band)] =
                    juce::jlimit (-maxCommandGainDb, maxCommandGainDb, commandGains[static_cast<size_t> (band)]);

                sections[static_cast<size_t> (band)] = makeSection (band, commandGains[static_cast<size_t> (band)]);
            }

            broadbandDb = broadbandBaseDb + commandGains[static_cast<size_t> (numBands)];
        };

        rebuild();

        //======================================================================
        // Gauss-Newton refinement against the realised cascade response, with
        // the interaction matrix as a fixed Jacobian (brief 3.3 step 3).
        auto maxError = 0.0f;

        for (int iteration = 0; iteration < maxGaussNewtonIterations; ++iteration)
        {
            std::array<float, numControlFrequencies> residual {};
            maxError = 0.0f;

            for (int j = 0; j < numControlFrequencies; ++j)
            {
                if (! controlUsable[static_cast<size_t> (j)])
                {
                    residual[static_cast<size_t> (j)] = 0.0f;
                    continue;
                }

                const auto realised = cascadeMagnitudeDb (sections, controlPoints[static_cast<size_t> (j)]) + broadbandDb;
                residual[static_cast<size_t> (j)] = target[static_cast<size_t> (j)] - realised;
                maxError = juce::jmax (maxError, std::abs (residual[static_cast<size_t> (j)]));
            }

            if (maxError < convergenceToleranceDb)
                break;

            // Refresh the Jacobian at the current operating point every few
            // steps; the sections' dB slopes change as their gains move.
            if (iteration % 4 == 0)
                refreshJacobian();

            std::array<float, numCommandGains> delta {};
            solveWithWorkingJacobian (residual, delta);

            for (int i = 0; i < numCommandGains; ++i)
                commandGains[static_cast<size_t> (i)] += delta[static_cast<size_t> (i)];

            rebuild();
        }

        //======================================================================
        // Stability projection - the guarantee, not an optimisation.
        //
        // The +/-10 dB shaping clamp bounds how far a band may deviate from
        // the broadband gain, but it does not by itself bound the *total*: a
        // pathological RT60(f) request against a short delay line can produce
        // a broadband attenuation of only a few dB, and a +10 dB shaping
        // deviation on top of that is net gain. A feedback delay network with
        // any frequency at which a line's round-trip gain reaches unity is
        // unstable, full stop, and no amount of well-behaved typical input
        // makes that safe.
        //
        // So the finished cascade is swept across a log-spaced grid (plus DC
        // and Nyquist) and, if its peak magnitude reaches unity, the broadband
        // gain is shifted down by exactly the excess. That is a uniform
        // translation: the fitted *shape* is preserved, every band's decay
        // simply becomes correspondingly shorter, and the network is
        // guaranteed strictly decaying at every frequency. For any request the
        // designer can actually honour the shift is zero.
        {
            auto peakDb = -1000.0f;

            for (const auto& point : stabilityPoints)
                peakDb = juce::jmax (peakDb, cascadeMagnitudeDb (sections, point) + broadbandDb);

            if (peakDb > -stabilityMarginDb)
                broadbandDb -= peakDb + stabilityMarginDb;
        }

        attenuation.sections = sections;
        attenuation.commandGainsDb = commandGains;
        attenuation.broadbandGain = juce::Decibels::decibelsToGain (broadbandDb);
        attenuation.commandGainsDb[static_cast<size_t> (numBands)] = broadbandDb;
        attenuation.maxFitErrorDb = maxError;

        return attenuation;
    }

    float DesignContext::peakMagnitudeDb (const LineAttenuation& attenuation) const
    {
        const auto broadbandDb = juce::Decibels::gainToDecibels (juce::jmax (1.0e-12f, attenuation.broadbandGain));
        auto peakDb = -1000.0f;

        for (const auto& point : stabilityPoints)
            peakDb = juce::jmax (peakDb, cascadeMagnitudeDb (attenuation.sections, point) + broadbandDb);

        return peakDb;
    }

    std::array<float, numBands> DesignContext::realisedT60 (const LineAttenuation& attenuation, int delaySamples) const
    {
        std::array<float, numBands> realised {};
        realised.fill (0.0f);

        if (! prepared || delaySamples <= 0)
            return realised;

        const auto broadbandDb = juce::Decibels::gainToDecibels (juce::jmax (1.0e-12f, attenuation.broadbandGain));

        for (int band = 0; band < numBands; ++band)
        {
            if (! bandUsable[static_cast<size_t> (band)])
                continue;

            const auto totalDb = cascadeMagnitudeDb (attenuation.sections, bandPoints[static_cast<size_t> (band)]) + broadbandDb;
            const auto perSampleDb = totalDb / static_cast<float> (delaySamples);

            if (perSampleDb >= -1.0e-12f)
                continue; // no attenuation at all: an infinite T60

            realised[static_cast<size_t> (band)] = static_cast<float> (-60.0 / (sampleRate * static_cast<double> (perSampleDb)));
        }

        return realised;
    }
}
