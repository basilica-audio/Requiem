#pragma once

#include <juce_dsp/juce_dsp.h>

// Requiem v0.3.0 wet-path workflow chain: a 12 dB/oct low-cut/high-cut pair
// followed by an input-follower ducker, applied to the *wet* signal only
// (after Width, before the DryWetMixer's wet input - the dry path is never
// touched). See the v0.3.0 brief section 3.4.
//
// NEUTRALITY GUARANTEE (the reason this class exists as its own unit, and
// what tests/WetEqDuckingTests.cpp pins): at the shipped defaults - Low Cut
// 20 Hz, High Cut 20 kHz, Duck 0% - process() is a *hard bypass*. Not "a
// filter whose response happens to be flat", but literally no filter run and
// no gain multiply: the wet samples are left exactly as they arrived. That
// is what lets a v0.2.0 session reload into v0.3.0 and render bit-identically
// even though the wet chain is structurally in the signal path.
//
// All state is allocated in prepare(); process() never allocates, locks, or
// calls anything that is not real-time safe.
class WetChain
{
public:
    WetChain() = default;

    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears filter/envelope state without deallocating. Safe on the audio
    // thread.
    void reset();

    // Processes `wet` in place. `dryMono` must point at `numSamples` samples
    // of the mono-summed *pre-everything* dry input - that is what drives
    // the ducker's envelope follower (ducking the reverb with the source, as
    // a cinematic dialog/percussion duck does), never the wet signal itself.
    // Passing nullptr disables ducking for this block.
    void process (juce::dsp::AudioBlock<float>& wet, const float* dryMono, int numSamples) noexcept;

    //==============================================================================
    // Real-time-safe parameter setters (targets only; every audible change is
    // smoothed internally). Safe to call every block from the audio thread.
    void setLowCutHz (float newLowCutHz);
    void setHighCutHz (float newHighCutHz);
    void setDuckAmountPercent (float newDuckAmountPercent);
    void setDuckAttackMs (float newAttackMs);
    void setDuckReleaseMs (float newReleaseMs);

    // True when the chain is currently a literal no-op (both cuts parked at
    // their bypass ends and the duck gain sitting at exactly 1.0), i.e. when
    // process() will not modify a single sample. Exposed for tests.
    bool isHardBypassed() const noexcept;

    //==============================================================================
    // Range ends at which the corresponding filter is hard-bypassed rather
    // than run with a nominally-flat response. These must match the Low Cut /
    // High Cut parameter range ends in ParameterLayout.cpp.
    static constexpr float lowCutBypassHz = 20.0f;
    static constexpr float highCutBypassHz = 20000.0f;

    // Reference envelope level the ducker normalises against: an envelope of
    // this magnitude (roughly -12 dBFS mean) produces the full commanded duck
    // depth. Chosen so a normally-levelled source hits full duck on peaks
    // without a quiet source ducking not at all.
    static constexpr float duckReferenceEnvelope = 0.25f;

    // Smoothing time applied to the commanded Duck *amount* (not to the
    // envelope-derived gain). Smoothing the amount rather than the final
    // gain is deliberate: it keeps a Duck automation sweep zipper-free while
    // leaving the duck's actual step response governed purely by the attack/
    // release coefficients, which is what makes those two parameters mean
    // what their names say (see tests/WetEqDuckingTests.cpp).
    static constexpr double duckAmountSmoothingSeconds = 0.005;

    // Smoothing time of the two cutoff frequencies.
    static constexpr double cutoffSmoothingSeconds = 0.02;

private:
    void updateEnvelopeCoefficients();

    double sampleRate = 44100.0;

    juce::dsp::StateVariableTPTFilter<float> lowCutFilter;
    juce::dsp::StateVariableTPTFilter<float> highCutFilter;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowCutSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highCutSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> duckAmountSmoothed;

    float lastLowCutHz = lowCutBypassHz;
    float lastHighCutHz = highCutBypassHz;
    float duckAmount01 = 0.0f;
    float duckAttackMs = 10.0f;
    float duckReleaseMs = 250.0f;

    // One-pole envelope-follower coefficients, derived from the attack/
    // release times: env[n] = a*env[n-1] + (1-a)*|x[n]|, with `a` selected
    // per sample by whether the rectified input is above or below the
    // current envelope.
    float attackCoefficient = 0.0f;
    float releaseCoefficient = 0.0f;
    float envelope = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WetChain)
};
