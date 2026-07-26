#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

// Offline analysis of an impulse response, feeding Requiem v0.3.0's Hybrid
// Tail and Tail Bloom engine modes (brief 3.2). Everything here runs on a
// background thread with no real-time constraints - it allocates, iterates
// over multi-second buffers, and calls transcendentals freely. Nothing in
// this namespace may ever be called from process().
//
// Three products, all consumed by the hybrid splice:
//
//   * MIXING TIME t_mix - the instant the early reflection pattern has
//     become statistically indistinguishable from Gaussian noise, i.e. the
//     point past which a measured IR carries no information a well-designed
//     FDN cannot synthesise. Estimated with Abel & Huang's normalised echo
//     density (AES 121, 2006, "A simple, robust measure of reverberation
//     echo density"): a sliding 25 ms Hann window, hop 5 ms, counting the
//     fraction of samples exceeding the window's own standard deviation and
//     normalising by erfc(1/sqrt(2)) - the expected fraction for Gaussian
//     noise, so NED -> 1 exactly when the signal has become Gaussian.
//
//   * RT60 PER OCTAVE BAND - Schroeder backward integration (JASA 37, 1965)
//     per octave band, with a linear regression over the -5...-35 dB span of
//     the energy decay curve, which is the ISO 3382 T30 convention. This is
//     what the FDN's per-line attenuation filters are fitted to, and it is
//     what makes "the tail decays like this IR does" a measurable claim
//     rather than a vibe.
//
//   * ENERGY DECAY RELIEF AT t_mix - the residual energy per octave band
//     from t_mix onward. The correction FIR is designed from the ratio of
//     this to the FDN branch's own measured residual energy (Carpentier,
//     Szpruch, Noisternig & Warusfel, DAFx-14, "Parametric control of
//     convolution-based room simulators", eq. 3), so the synthesised tail
//     picks up with the same spectrum the truncated convolution handed off.
namespace IrAnalysis
{
    // Ten octave bands, 31.25 Hz - 16 kHz. Bands whose centre is too close
    // to Nyquist for the sample rate in use are reported as invalid rather
    // than silently measured through a degenerate filter.
    inline constexpr int numOctaveBands = 10;

    // Linear-phase correction FIR length. 256 taps at 48 kHz is ~5.3 ms,
    // enough resolution to correct a per-octave spectral tilt, and its
    // 128-sample group delay is compensated in the FDN branch's pre-delay
    // (brief 3.2 - together with the FDN's own intrinsic onset delay).
    inline constexpr int correctionFirLength = 256;
    inline constexpr int correctionFirGroupDelay = correctionFirLength / 2;

    // Mixing-time clamps (brief 3.2). Below 50 ms there is not enough early
    // field left to be worth convolving; above 350 ms the convolution cost
    // the hybrid mode exists to save stops being saved.
    inline constexpr float minMixingTimeSeconds = 0.050f;
    inline constexpr float maxMixingTimeSeconds = 0.350f;

    // Normalised echo density at which the field counts as fully diffuse.
    inline constexpr float diffuseNedThreshold = 0.95f;

    // Length of the raised-cosine fade with which the convolution kernel is
    // truncated at t_mix (brief 3.3). h_ER = h * w and h_TL = h * (1 - w)
    // reconstruct h exactly, which is the basis of the splice null test.
    inline constexpr float spliceFadeSeconds = 0.010f;

    // Below this per-band regression quality the IR's decay is not usefully
    // exponential (gated reverbs, truncated captures, non-IR audio). The
    // hybrid splice degrades gracefully for such IRs - see Analysis::
    // hasLowConfidence.
    inline constexpr float minRegressionRSquared = 0.9f;
    inline constexpr int lowConfidenceBandCount = 3;

    //==========================================================================
    struct Analysis
    {
        float mixingTimeSeconds = minMixingTimeSeconds;

        // Per-octave RT60 in seconds. Bands flagged invalid (see
        // bandValid) hold the nearest valid band's value so downstream
        // consumers never see a zero or a NaN.
        std::array<float, numOctaveBands> rt60Octave {};

        // Per-octave residual energy from mixingTimeSeconds onward, as a
        // linear energy sum (not dB, not normalised).
        std::array<float, numOctaveBands> edrAtMixingTime {};

        // Per-octave coefficient of determination of the -5...-35 dB
        // regression. Low values mean the band does not decay exponentially.
        std::array<float, numOctaveBands> regressionRSquared {};

        std::array<bool, numOctaveBands> bandValid {};

        // True when three or more bands regressed poorly - the IR's decay is
        // not usefully exponential, so a hybrid splice fitted to it would
        // audibly mis-track. ReverbEngine treats such IRs conservatively
        // (brief section 7, risk 2).
        bool hasLowConfidence = false;
    };

    //==========================================================================
    // Octave-band centre frequencies, 31.25 Hz doubling to 16 kHz.
    std::array<float, numOctaveBands> octaveCentreFrequencies() noexcept;

    // True when `centreHz` is low enough relative to `sampleRate` for the
    // measurement bandpass to be well-conditioned.
    bool isBandUsable (float centreHz, double sampleRate) noexcept;

    // Filters `input` (length `numSamples`) into the octave band centred on
    // `centreHz` and writes the result to `output` (same length). A cascade
    // of two identical RBJ bandpass biquads (Q = 1.4142, i.e. one octave),
    // giving a 4th-order band. Shared between the analysis here and the
    // measurement code in tests/ so an RT60 assertion is measured through
    // exactly the filter the fit was made against.
    void filterOctaveBand (const float* input, float* output, int numSamples,
                            float centreHz, double sampleRate);

    // Abel-Huang normalised echo density at each hop position, plus the
    // estimated mixing time (already clamped to [minMixingTimeSeconds,
    // maxMixingTimeSeconds]). `nedCurveOut`, if non-null, receives the raw
    // NED curve sampled every `hopSeconds`.
    float estimateMixingTimeSeconds (const float* data, int numSamples, double sampleRate,
                                      std::vector<float>* nedCurveOut = nullptr);

    // Normalised echo density of one window of samples, for tests and for
    // the tail-diffuseness assertion (brief test 6.9).
    float normalisedEchoDensity (const float* data, int numSamples);

    // Schroeder backward-integration RT60 of an already band-filtered
    // signal. Returns 0 and leaves `rSquaredOut` at 0 if the band never
    // decays through the -35 dB point within the buffer.
    float schroederRt60Seconds (const float* bandData, int numSamples, double sampleRate,
                                 float* rSquaredOut = nullptr);

    // Full analysis of an arbitrary (user-loaded or procedural) IR.
    Analysis analyse (const juce::AudioBuffer<float>& impulseResponse, double sampleRate);

    // Procedural fast path (brief 3.2): for Requiem's own generator the
    // per-band RT60 is analytic - mid = Decay, low = Decay * BassDecay, high
    // = 0.8 * Decay with the terminal corner at Damping - so the Schroeder
    // fit is skipped and rt60Octave is synthesised directly. The NED/EDR
    // analysis still runs against the real buffer, because the correction
    // FIR and the mixing time depend on the actual early field.
    Analysis analyseProcedural (const juce::AudioBuffer<float>& impulseResponse, double sampleRate,
                                 float decaySeconds, float dampingHz, float bassDecayMultiplier,
                                 float highBandDecayMultiplier);

    // Per-octave residual energy of `data` from `startSample` to the end.
    std::array<float, numOctaveBands> octaveBandEnergies (const float* data, int numSamples,
                                                            int startSample, double sampleRate);

    // Designs a 256-tap linear-phase FIR whose magnitude response follows
    // `perBandGains` (linear, one per octave band), 1/3-octave smoothed
    // across the band boundaries. Group delay is exactly
    // correctionFirGroupDelay samples.
    std::array<float, correctionFirLength> designCorrectionFir (const std::array<float, numOctaveBands>& perBandGains,
                                                                  double sampleRate);

    // The raised-cosine splice window w[n] used to truncate the convolution
    // kernel at the mixing time: 1 before the fade, cos^2-shaped through the
    // fade, 0 after. h_ER = h * w and h_TL = h * (1 - w) sum back to h
    // exactly (brief test 6.9).
    float spliceWindow (int sampleIndex, double sampleRate, float mixingTimeSeconds) noexcept;
}
