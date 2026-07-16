#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Procedural impulse-response generation for Requiem's convolution reverb.
// Deliberately independent of juce::dsp::Convolution and of
// juce::AudioProcessor so it is directly unit-testable (see
// tests/ImpulseResponseGeneratorTests.cpp) and so it can run entirely off
// the audio thread: generation involves heap allocation (the returned
// AudioBuffer) and per-sample trig/exp calls, neither of which are
// real-time safe. Callers (ReverbEngine) only ever invoke this from
// prepare() or from a message-thread-only regeneration path - never from
// process().
//
// v0.2.0 research-derived rework (see docs/design-brief.md and
// docs/research-notes.md for the full sourcing): replaces v0.1's two
// defects -
//
//   1. Early reflections used to place a full-gain tap at sample 0 and let
//      every subsequent tap decay away geometrically across a fixed window.
//      Every researched reference instead builds reflection *density* up
//      over the first tens of milliseconds, then holds roughly flat energy
//      through ~160 ms, before handing off to the diffuse late tail - see
//      addEarlyReflections() below.
//   2. Damping used to be a single, static one-pole low-pass applied
//      uniformly to the entire tail from t = 0, so the tail's spectral
//      balance never changed over its life - only its level did. The tail
//      is now split into low/mid/high bands (crossovers at ~500 Hz/~5 kHz)
//      with independent decay rates (bassDecay scales the low band; the
//      high band gets an implicit faster decay and a progressively
//      descending cutoff so the tail measurably darkens as it decays) - see
//      generateProceduralImpulseResponse() below.
//
// Approach otherwise unchanged from v0.1: per channel, decorrelated white
// noise (a distinct juce::Random stream per channel, for stereo width in
// the tail) drives both the diffuse late-tail layer and a discrete
// early-reflection tap layer, blended via an Early/Late Balance
// equal-power crossfade. This is a simple but standard "filtered noise
// burst" (plus a discrete early-reflection train) algorithmic-reverb IR
// model - not a physically modelled room, but sufficient for a cinematic
// wash of cathedral/hall/chamber-style tails.
namespace ReverbIR
{
    // Decay is clamped to this range before sizing the generated buffer,
    // bounding both memory and the convolution engine's CPU cost regardless
    // of what a caller (or a corrupt saved state) requests.
    constexpr float minDecaySeconds = 0.1f;
    constexpr float maxDecaySeconds = 10.0f;

    // Damping is clamped to this range; the upper bound is also re-clamped
    // below Nyquist internally for whatever sample rate is passed in. As of
    // v0.2.0, Damping sets only the *terminal* HF corner the high band's
    // progressively descending cutoff settles at, not a static filter
    // applied uniformly to the whole tail - see docs/design-brief.md.
    constexpr float minDampingHz = 20.0f;
    constexpr float maxDampingHz = 20000.0f;

    // Size (new in v0.2.0): continuous, decoupled from Decay. Scales the
    // early-reflection buildup/flat-window timing within each Space
    // choice's own envelope (see characteristicsFor() in the .cpp) -
    // research-derived from Bricasti/Altiverb's documented existence of a
    // decoupled Size control separate from Decay (RT60) and Space
    // (reflection character). Must never measurably change RT60 - see
    // tests/ImpulseResponseGeneratorTests.cpp's Size-decoupling test.
    constexpr float minSize01 = 0.0f;
    constexpr float maxSize01 = 1.0f;
    constexpr float defaultSize01 = 0.5f;

    // Bass Decay (new in v0.2.0): 25-175% multiplier on RT60 for the low
    // band only (below the ~500 Hz crossover). Default 130% (bass rings
    // measurably longer than mid/high by default, matching the documented
    // real-hall "RT60 = 2.0s at 125Hz vs 0.8s at 1kHz" ratio - see
    // docs/research-notes.md section 3). Range/style directly modelled on
    // LiquidSonics Reverberate's documented 25-175% band-decay-multiplier
    // convention.
    constexpr float minBassDecayMultiplier = 0.25f;
    constexpr float maxBassDecayMultiplier = 1.75f;
    constexpr float defaultBassDecayMultiplier = 1.3f;

    // Implicit (non-parameterized) decay multiplier for the high band -
    // highs finish measurably before the mid band, matching the same
    // real-hall HF-absorption finding. Not exposed as a separate knob to
    // keep the v0.2.0 parameter count sane (see docs/design-brief.md).
    constexpr float highBandDecayMultiplier = 0.8f;

    // Multiband crossover frequencies (research-derived, matching
    // LiquidSonics Reverberate's documented ~600 Hz/~6 kHz precedent -
    // see docs/research-notes.md section 3). Re-clamped below Nyquist
    // internally for low sample rates.
    constexpr float lowMidCrossoverHz = 500.0f;
    constexpr float midHighCrossoverHz = 5000.0f;

    // Shapes the discrete early-reflection layer generated ahead of the
    // diffuse late tail (see generateProceduralImpulseResponse() below).
    // Space does *not* change the late diffuse tail itself - that stays
    // governed purely by Decay/Damping/BassDecay, as in the v0.1 model -
    // only the density/spread/character of the early reflections layered on
    // top of it. Cathedral = long, dense, widely spread reflections; Hall =
    // the balanced default character; Chamber = short, sparse, tight
    // reflections.
    enum class SpaceType
    {
        cathedral,
        hall,
        chamber
    };

    // Generates a procedural impulse response.
    //
    // `numChannels` is clamped to [1, 2] (mono or stereo); `seed` lets tests
    // (and A/B channel decorrelation) get reproducible, distinct noise
    // sequences.
    //
    // `earlyLateBalance01` (clamped to [0, 1]) is an equal-power crossfade
    // between the discrete early-reflection layer (1.0 gain at 0.0) and the
    // diffuse late tail (1.0 gain at 1.0); the default (0.8) keeps the
    // diffuse tail dominant, close to the v0.1-era character, while still
    // giving the early layer some presence. As of v0.2.0 the crossfade point
    // internally respects the early-reflection buildup/flat-window handoff
    // timing (see the .cpp) rather than blending two independently-shaped
    // layers irrespective of that timing.
    //
    // `size01` (clamped to [0, 1], default 0.5) scales the early-reflection
    // buildup/flat-window timing within Space's own envelope - see
    // minSize01/maxSize01/defaultSize01 above. Independent of Decay/RT60.
    //
    // `bassDecayMultiplier` (clamped to [0.25, 1.75], default 1.3) scales
    // the low band's (< ~500 Hz) RT60 relative to the mid band's - see
    // minBassDecayMultiplier/maxBassDecayMultiplier/defaultBassDecayMultiplier
    // above.
    //
    // `freeze`, when true, replaces the usual per-band RT60-style
    // exponential decay envelopes with a flat (non-decaying) one at full
    // gain for every band, so the tail's spectral content sustains for the
    // full generated buffer length instead of dying away - and suppresses
    // the early-reflection layer entirely, for a clean sustained pad/wash.
    // The high band's progressively descending cutoff is also disabled
    // while frozen (a static terminal-Damping cutoff is used instead), so a
    // frozen texture holds one consistent spectral color rather than
    // continuing to darken while sustained - this preserves v0.1's Freeze
    // envelope-flatness guarantee exactly (see
    // tests/ImpulseResponseGeneratorTests.cpp). Both `freeze` and the flat
    // envelope override `earlyLateBalance01` unconditionally while frozen (a
    // frozen pad is always the full sustained diffuse wash, never a
    // sustained early-reflection pattern). The buffer is still bounded to
    // `decaySeconds` worth of samples (a convolution engine processes a
    // finite kernel, so this is a bounded sustain, not a literal infinite
    // loop) - see docs/architecture.md.
    //
    // Not real-time safe - allocates the returned buffer and does
    // per-sample exp()/random calls. Call only from prepare() or a
    // message-thread-only regeneration path, never from process().
    juce::AudioBuffer<float> generateProceduralImpulseResponse (double sampleRate,
                                                                  float decaySeconds,
                                                                  float dampingHz,
                                                                  int numChannels,
                                                                  SpaceType space = SpaceType::hall,
                                                                  float earlyLateBalance01 = 0.8f,
                                                                  bool freeze = false,
                                                                  int seed = 1,
                                                                  float size01 = defaultSize01,
                                                                  float bassDecayMultiplier = defaultBassDecayMultiplier);
}
