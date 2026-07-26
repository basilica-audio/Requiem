#pragma once

// Central definition of all AudioProcessorValueTreeState parameter IDs for
// Requiem. See docs/architecture.md for the corresponding signal-flow
// diagram.
//
// FROZEN AS OF THE v0.1.0 PARAMETER LAYOUT (the complete list below,
// including the M1 additions - space/earlyLateBalance/modulation/freeze):
// Parameter IDs below must NEVER change once shipped - saved sessions and
// presets persist the APVTS state keyed by these string IDs, and renaming or
// removing one would silently break every user's saved state. Ranges,
// defaults, and skew MAY still be refined during voicing/tuning milestones;
// only the IDs themselves are frozen.
namespace ParamIDs
{
    // Reverb time: controls both the length of the procedurally generated
    // impulse response and its RT60-style exponential decay envelope.
    inline constexpr auto decay = "decay";

    // Delay, in milliseconds, before the wet (convolved) signal begins -
    // separates the direct sound from the onset of the reverb tail.
    inline constexpr auto preDelay = "preDelay";

    // HF damping cutoff: the one-pole low-pass frequency applied to the
    // procedural impulse response's filtered-noise tail. Higher = brighter/
    // less damped tail, lower = darker/more damped.
    inline constexpr auto damping = "damping";

    // Stereo width applied to the wet signal only, via mid/side scaling.
    // 0 = mono, 100 = the convolution engine's natural stereo image,
    // 200 = exaggerated/extra-wide.
    inline constexpr auto width = "width";

    // Dry/wet mix. At 0% the plugin is a delay-compensated passthrough of
    // the input (see ReverbEngine's DryWetMixer usage).
    inline constexpr auto mix = "mix";

    // Output trim, applied after the dry/wet mix.
    inline constexpr auto output = "output";

    // Space character: shapes the discrete early-reflection layer of the
    // procedurally generated impulse response (Cathedral/Hall/Chamber - see
    // ReverbIR::SpaceType). Does not affect the diffuse late tail, which is
    // still governed purely by Decay/Damping.
    inline constexpr auto space = "space";

    // Equal-power crossfade between the early-reflection layer (0%) and the
    // diffuse late tail (100%) baked into the generated impulse response.
    inline constexpr auto earlyLateBalance = "earlyLateBalance";

    // Depth of a subtle post-convolution chorus-style modulation applied to
    // the wet tail only (juce::dsp::Chorus), to soften metallic ringing/add
    // richness. 0% is a bit-identical passthrough of the unmodulated tail.
    inline constexpr auto modulation = "modulation";

    // Sustains the tail's current spectral content instead of letting it
    // decay, by regenerating the impulse response with a flat envelope
    // (see ReverbIR::generateProceduralImpulseResponse's freeze parameter).
    inline constexpr auto freeze = "freeze";

    // v0.2.0 additions (see docs/design-brief.md). New parameter IDs are
    // appended here, never inserted between existing ones - see the FROZEN
    // note above.

    // Apparent size of the space, decoupled from Decay (RT60) and Space
    // (reflection character) - scales the early-reflection buildup/flat-
    // window timing within Space's own envelope (see
    // ReverbIR::generateProceduralImpulseResponse's size01 parameter).
    // Independent of Decay: sweeping Size must not measurably change RT60.
    inline constexpr auto size = "size";

    // Multiplier (25-175%) on RT60 for the low band (< ~500 Hz) only,
    // relative to the mid band's RT60 (see
    // ReverbIR::generateProceduralImpulseResponse's bassDecayMultiplier
    // parameter). Bass rings longer than mid/high by default (130%),
    // matching real-hall low-frequency decay measurements.
    inline constexpr auto bassDecay = "bassDecay";

    // v0.3.0 "Living Tail" additions (see the v0.3.0 implementation brief).
    // Appended after the v0.2.0 parameters, never inserted between existing
    // ones - see the FROZEN note above. Every one of these defaults to a
    // neutral value: either a hard-bypass setting (lowCut/highCut/
    // duckAmount) or a value that is inaudible because engineMode defaults
    // to Classic Convolution, which is the bit-identical v0.2.0 engine.

    // Engine topology: Classic Convolution (index 0, the bit-identical
    // v0.2.0 engine and the default), Hybrid Tail (index 1, convolution
    // early field spliced at the analysed mixing time into an auto-fitted
    // feedback-delay-network late field), or Tail Bloom (index 2, the full
    // convolution plus an FDN "bloom" layer summed on top). Order must
    // match ReverbEngine::EngineMode.
    inline constexpr auto engineMode = "engineMode";

    // Tail modulation topology for the two FDN modes: Matrix (index 0,
    // time-varying orthogonal Givens rotations - pitch-stable by
    // construction), Lush (index 1, interpolated modulated delay reads -
    // deliberately vintage, audibly detunes), or Off (index 2). Inaudible
    // in Classic mode.
    inline constexpr auto tailModMode = "tailModMode";

    // Depth of the FDN tail modulation, 0-100%. Maps to 0-6 degrees of
    // Givens rotation in Matrix mode, or 0-3 samples of delay deviation in
    // Lush mode. Inaudible in Classic mode.
    inline constexpr auto tailModDepth = "tailModDepth";

    // Rate scaling of the FDN tail modulation LFOs, 25-400% of their
    // nominal randomised base rates. Inaudible in Classic mode.
    inline constexpr auto tailModRate = "tailModRate";

    // Level of the FDN "bloom" branch summed on top of the full-length
    // convolution in Tail Bloom mode, 0-100% (soft-tapered: branch gain is
    // the square of the normalised amount). Inaudible in the other two
    // modes.
    inline constexpr auto bloomAmount = "bloomAmount";

    // Wet-path post-EQ low cut (12 dB/oct high-pass), 20-2000 Hz. 20 Hz is
    // a hard bypass - the filter is not run at all, so the default path is
    // bit-identical to v0.2.0.
    inline constexpr auto lowCut = "lowCut";

    // Wet-path post-EQ high cut (12 dB/oct low-pass), 1000-20000 Hz.
    // 20000 Hz is a hard bypass, as above.
    inline constexpr auto highCut = "highCut";

    // Amount by which the (dry) input signal ducks the wet signal,
    // 0-100%. 0% is a bit-identical bypass - the wet gain is exactly 1.
    inline constexpr auto duckAmount = "duckAmount";

    // Attack time of the ducker's envelope follower, 1-200 ms. Inert
    // while duckAmount is 0%.
    inline constexpr auto duckAttack = "duckAttack";

    // Release time of the ducker's envelope follower, 50-2000 ms. Inert
    // while duckAmount is 0%.
    inline constexpr auto duckRelease = "duckRelease";
}

// Not an APVTS parameter (it's a string, not automatable) - the path of an
// optional user-loaded impulse response file, persisted alongside the APVTS
// state as a plain XML attribute. See PluginProcessor::getStateInformation/
// setStateInformation and ReverbEngine::loadUserImpulseResponse.
namespace StateKeys
{
    inline constexpr auto userIrPath = "userIrPath";
}
