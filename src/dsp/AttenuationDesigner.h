#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

#include "IrAnalysis.h"

// Design of the per-delay-line attenuation filters that give Requiem's FDN
// its frequency-dependent decay (brief 3.3). This is the part of the engine
// that turns "the tail decays like the analysed IR" from a claim into a
// measurable, test-enforced property.
//
// THE PROBLEM. In a lossless FDN the modal decay of the whole network is set
// entirely by the per-line attenuation: a line of m samples must attenuate by
// exactly m * delta(omega) dB per traversal, where
//
//     delta(omega) = -60 / (fs * T60(omega))       dB per sample
//
// Getting that wrong by a fraction of a dB matters enormously near
// delta ~ 0: at a 3 s T60 and 48 kHz, delta is -4.2e-4 dB/sample, so a 0.2 dB
// error on a 1776-sample line moves T60 by more than a second, and an error
// of the wrong sign makes the network unstable outright.
//
// THE APPROACH (Schlecht & Habets, DAFx-17, "Accurate reverberation time
// control in feedback delay networks"; refined by Prawda, Schlecht &
// Valimaki, DAFx-19). Each line gets a cascaded graphic EQ - one low shelf,
// eight peaking sections on octave centres, one high shelf - plus a
// broadband gain, eleven command gains in total. The sections interact, so
// the command gains cannot simply be read off the target: instead an
// interaction matrix B is built once per sample rate (the dB response of each
// unit-gain section at each of nineteen log-spaced control frequencies), its
// least-squares pseudo-inverse is precomputed by Householder QR, and the
// command gains are then refined by Gauss-Newton iterations against the
// realised cascade response.
//
// STABILITY. Command gains are clamped to +/-10 dB, which is the paper's
// zero-instability guarantee: the shaping deviation around the broadband
// gain can never push a band above unity loop gain no matter how pathological
// the requested T60(f) curve is. tests/FdnTailTests.cpp runs the paper's
// Monte Carlo (50 seeded random RT60 curves) against exactly that guarantee.
//
// THREADING. Everything here is background-thread work: it allocates,
// iterates, and calls transcendentals. The audio thread only ever consumes
// the finished LineAttenuation structs, handed over RCU-style.
namespace AttenuationDesign
{
    // One low shelf + eight peaks + one high shelf, on the same octave
    // centres IrAnalysis measures RT60 on.
    inline constexpr int numBands = IrAnalysis::numOctaveBands;

    // The eleventh gain is the broadband one. Band gains are kept centred
    // around it so the +/-10 dB clamp always bites on the *shaping*
    // deviation, never on the overall decay rate.
    inline constexpr int numCommandGains = numBands + 1;

    // Band centres plus geometric midpoints: fitting only at the centres
    // would leave the response free to wander between them.
    inline constexpr int numControlFrequencies = 2 * numBands - 1;

    // The DAFx-17 zero-instability clamp.
    inline constexpr float maxCommandGainDb = 10.0f;

    inline constexpr int maxGaussNewtonIterations = 50;

    // Size of the grid on which the finished cascade's magnitude is verified
    // to be strictly below unity. The grid is not purely log-spaced: it
    // explicitly includes DC, Nyquist, every section's own centre frequency
    // and every control frequency, because that is where a cascade of shelving
    // and peaking sections actually peaks. A purely log-spaced grid straddles
    // those maxima and under-reads them by a few hundredths of a dB - which is
    // precisely the size of the margin being enforced.
    inline constexpr int stabilityGridSize = 96;

    // How far below unity the realised per-traversal gain is held. Small
    // enough not to colour a well-conditioned design, large enough to swallow
    // both float rounding in the biquad cascade and any residual grid
    // interpolation error.
    inline constexpr float stabilityMarginDb = 0.05f;

    // Stop refining once every control frequency is within this of target;
    // far below the 5% T60 error test 6.7 asserts.
    inline constexpr float convergenceToleranceDb = 0.001f;

    //==========================================================================
    // Precomputed cos/sin of omega and 2*omega for one evaluation frequency.
    // The design loop evaluates the same nineteen control frequencies
    // thousands of times per solve, so hoisting the four transcendentals out
    // of the inner loop is what makes a full sixteen-line re-solve cheap
    // enough to run at the brief's ~100 Hz control-rate ramp.
    struct FrequencyPoint
    {
        float cosOmega = 1.0f, cosTwoOmega = 1.0f, sinOmega = 0.0f, sinTwoOmega = 0.0f;

        static FrequencyPoint fromHertz (float frequencyHz, double sampleRate) noexcept;
    };

    // A direct-form biquad, stored as plain floats so the audio thread never
    // touches a reference-counted coefficient object.
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

        // Magnitude response in dB at a precomputed evaluation frequency.
        float magnitudeDb (const FrequencyPoint& point) const noexcept;
    };

    // The complete attenuation of one delay line: a ten-section cascade plus
    // a broadband linear gain.
    struct LineAttenuation
    {
        std::array<Biquad, numBands> sections {};
        float broadbandGain = 1.0f;
        std::array<float, numCommandGains> commandGainsDb {};

        // Worst-case realised-vs-target error over the control grid, in dB.
        // Surfaced for the designer's own unit test.
        float maxFitErrorDb = 0.0f;
    };

    //==========================================================================
    // Sample-rate-dependent design context: the control-frequency grid, the
    // interaction matrix, and its precomputed least-squares pseudo-inverse.
    // Build once per sample rate (prepare()), then reuse for every line and
    // every T60 re-solve - a continuous Decay drag is only a re-solve, never
    // a rebuild.
    class DesignContext
    {
    public:
        DesignContext() = default;

        void prepare (double newSampleRate);

        double getSampleRate() const noexcept { return sampleRate; }
        bool isPrepared() const noexcept { return prepared; }

        // Designs the attenuation for a single delay line of `delaySamples`
        // samples against a per-octave T60 target, in seconds.
        LineAttenuation design (const std::array<float, numBands>& t60Seconds, int delaySamples) const;

        // Realised T60 per octave band implied by a designed cascade, for the
        // designer's unit test (brief test 6.7) - the inverse of design().
        std::array<float, numBands> realisedT60 (const LineAttenuation& attenuation, int delaySamples) const;

        const std::array<bool, numBands>& getBandUsable() const noexcept { return bandUsable; }

        // Peak magnitude of a designed cascade (sections plus broadband gain)
        // in dB, over the stability grid. Strictly negative is the network's
        // stability condition; tests/FdnTailTests.cpp asserts it directly
        // rather than inferring it from a rendered decay.
        float peakMagnitudeDb (const LineAttenuation& attenuation) const;

    private:
        Biquad makeSection (int band, float gainDb) const;
        float cascadeMagnitudeDb (const std::array<Biquad, numBands>& sections, const FrequencyPoint& point) const;
        void solveLeastSquares (const std::array<float, numControlFrequencies>& residual,
                                 std::array<float, numCommandGains>& deltaOut) const;

        double sampleRate = 0.0;
        bool prepared = false;

        std::array<float, numBands> bandCentres {};
        std::array<bool, numBands> bandUsable {};
        std::array<float, numControlFrequencies> controlFrequencies {};
        std::array<FrequencyPoint, numControlFrequencies> controlPoints {};
        std::array<bool, numControlFrequencies> controlUsable {};
        std::array<FrequencyPoint, numBands> bandPoints {};
        std::array<FrequencyPoint, stabilityGridSize> stabilityPoints {};

        // Row-major pseudo-inverse of the interaction matrix,
        // numCommandGains x numControlFrequencies.
        std::vector<float> pseudoInverse;
    };

    //==========================================================================
    // Required per-sample attenuation, in dB, for a given T60. Negative.
    float perSampleAttenuationDb (float t60Seconds, double sampleRate) noexcept;
}
