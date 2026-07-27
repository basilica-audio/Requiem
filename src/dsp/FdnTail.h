#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

#include "AttenuationDesigner.h"

// Requiem v0.3.0's late-field engine: a sixteen-line feedback delay network
// with a lossless orthogonal prototype, per-line frequency-dependent
// attenuation fitted to a measured RT60(f) curve, pitch-stable time-varying
// modulation, and a true structural freeze (brief 3.3).
//
// TOPOLOGY (Jot & Chaigne 1991; Schlecht & Habets, "Feedback delay networks
// in artificial reverberation and reverberation enhancement", JAES 2020):
//
//     s_i(n)          = output of delay line i
//     y_{L,R}(n)      = c_{L,R}^T s(n)
//     line i input    = b_i x(n) + sum_j A_ij(n) g_j(z){ s_j(n) }
//
// with A(n) = G(n) H, H the Householder reflection I - (2/16) 1 1^T and G(n)
// a product of eight Givens rotations on disjoint index pairs. Both factors
// are orthogonal at every instant, so the prototype is lossless at every
// instant - the network cannot be destabilised by modulation, and the
// modulation cannot shift pitch, because no delay length ever changes.
// That is the Schlecht & Habets (JASA 138, 2015, "Time-varying feedback
// matrices in feedback delay networks and their application in artificial
// reverberation") result this engine is built on, and it is what
// tests/FdnTailTests.cpp's sub-cent pitch assertion pins.
//
// Note that the output reads only delay-line outputs - there is no direct
// feedthrough term - so the network emits its first non-zero sample only
// after its *shortest* delay line. ReverbEngine compensates for exactly that
// (plus the correction FIR's group delay) when it places the hybrid splice;
// see getShortestDelaySamples().
//
// MODULATION MODES
//   * Matrix (default): the Givens angles move, the delay lengths do not.
//     Zero pitch modulation by construction.
//   * Lush: cubic-Hermite interpolated fractional delay reads with per-line
//     LFOs. Deliberately vintage, and deliberately *does* detune - the test
//     suite asserts that it exceeds one cent, which is what keeps the two
//     modes from being silently wired to the same code path.
//   * Off.
//
// FREEZE: the per-line attenuation is crossfaded out to unity and the input
// taps to zero over 20 ms, directly from the parameter change on the audio
// thread. What is left is the lossless prototype, which holds the audio
// already circulating in the delay lines indefinitely and exactly - no IR
// regeneration, no timer, no decay. This is the structural advantage an FDN
// has over a convolution engine, and it is instant: toggle latency is under
// one block.
//
// THREADING: prepare() allocates everything. process() and every setter are
// real-time safe. New attenuation coefficients arrive from the design thread
// through a SpinLock-guarded slot that process() only ever *tries* - the same
// wait-free hand-off pattern ReverbEngine has used for impulse responses
// since v0.1.1.
class FdnTail
{
public:
    enum class ModulationMode
    {
        matrix = 0,
        lush = 1,
        off = 2,
    };

    // Sixteen lines: dense enough that the modal density at 63 Hz is well
    // above the audible echo threshold, sparse enough that the whole network
    // costs a fraction of the convolution it replaces.
    static constexpr int numLines = 16;

    // Log-spaced delay range. The lower bound doubles as the network's
    // intrinsic onset delay (see getShortestDelaySamples()).
    static constexpr float minDelayMs = 37.0f;
    static constexpr float maxDelayMs = 143.0f;

    // Maximum Givens rotation angle at full Tail Mod Depth, in degrees.
    static constexpr float maxRotationDegrees = 6.0f;

    // Lush-mode delay deviation at full depth, in samples at 48 kHz
    // (rescaled with the sample rate so the *musical* depth is rate-independent).
    static constexpr float maxLushDepthSamplesAt48k = 3.0f;

    static constexpr float minLushRateHz = 0.1f;
    static constexpr float maxLushRateHz = 1.2f;
    static constexpr float minMatrixRateHz = 0.05f;
    static constexpr float maxMatrixRateHz = 0.6f;

    // Rotation angles (and Lush LFO positions) are updated once per
    // sub-block rather than per sample - a 32-sample grid is far finer than
    // any modulation rate in use here and keeps the trig out of the sample loop.
    static constexpr int modulationSubBlockSamples = 32;

    static constexpr double freezeRampSeconds = 0.02;

    //==============================================================================
    FdnTail();

    // Not real-time safe: allocates the delay lines and re-primes their
    // lengths for the sample rate.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all delay-line and filter state. Safe on the audio thread.
    void reset();

    // Replaces `block` with the network's stereo (or mono) output for that
    // input. Never allocates.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    //==============================================================================
    // Design-thread only. Hands a freshly designed per-line attenuation set
    // over for process() to pick up on its next block, wait-free.
    void postAttenuation (const std::array<AttenuationDesign::LineAttenuation, numLines>& attenuation) noexcept;

    // Message/design-thread only, and only while processing is suspended.
    void setAttenuationImmediately (const std::array<AttenuationDesign::LineAttenuation, numLines>& attenuation);

    //==============================================================================
    // Real-time-safe setters.
    void setModulationMode (ModulationMode newMode) noexcept;
    void setModulationDepth (float newDepth01) noexcept;
    void setModulationRateScale (float newScale01Based) noexcept;

    // Engages/disengages the structural freeze. Takes effect within one
    // block, ramped over freezeRampSeconds.
    void setFrozen (bool shouldFreeze) noexcept;
    bool isFrozen() const noexcept { return frozen; }

    //==============================================================================
    // Delay-line lengths in samples, re-primed for the prepared sample rate.
    const std::array<int, numLines>& getDelayLengths() const noexcept { return delayLengths; }

    // Length of the shortest delay line, i.e. the number of samples between
    // an impulse arriving at the input and the first non-zero output sample.
    int getShortestDelaySamples() const noexcept { return shortestDelaySamples; }

    // Total delay across all lines, in samples - the classic modal-density
    // figure of merit.
    int getTotalDelaySamples() const noexcept;

private:
    void primeDelayLengths (double newSampleRate);
    void updateModulation() noexcept;
    void fetchPendingAttenuationIfAny() noexcept;
    float readLine (int line, float delaySamples) const noexcept;

    double sampleRate = 44100.0;
    int numChannels = 2;

    std::array<int, numLines> delayLengths {};
    std::array<std::vector<float>, numLines> lineBuffers {};
    std::array<int, numLines> writePositions {};
    int shortestDelaySamples = 1;

    // Per-line attenuation: ten biquad sections plus a broadband gain,
    // with two state words per section (transposed direct form II).
    std::array<AttenuationDesign::LineAttenuation, numLines> attenuationSets {};
    std::array<std::array<std::array<float, 2>, AttenuationDesign::numBands>, numLines> filterStates {};

    bool hasDesignedAttenuation = false;

    juce::SpinLock pendingAttenuationLock;
    std::array<AttenuationDesign::LineAttenuation, numLines> pendingAttenuation {};
    bool hasPendingAttenuation = false;

    // Injection and output tap vectors, +/-1 scaled by 1/sqrt(N). c_L and c_R
    // are mutually orthogonal, which is what decorrelates the two output
    // channels into a true-stereo tail rather than a doubled mono one.
    std::array<float, numLines> injectionTaps {};
    std::array<float, numLines> outputTapsLeft {};
    std::array<float, numLines> outputTapsRight {};

    // Modulation state. Eight disjoint index pairs, each with its own seeded
    // rate and phase so the network never settles into a periodic pattern.
    std::array<float, numLines / 2> rotationRatesHz {};
    std::array<float, numLines / 2> rotationPhases {};
    std::array<float, numLines / 2> rotationCos {};
    std::array<float, numLines / 2> rotationSin {};

    std::array<float, numLines> lushRatesHz {};
    std::array<float, numLines> lushPhases {};
    std::array<float, numLines> lushOffsets {};

    double modulationPhaseIncrementBase = 0.0;
    int samplesUntilModulationUpdate = 0;

    ModulationMode modulationMode = ModulationMode::matrix;
    float modulationDepth01 = 0.4f;
    float modulationRateScale = 1.0f;

    bool frozen = false;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> freezeAmount;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FdnTail)
};
