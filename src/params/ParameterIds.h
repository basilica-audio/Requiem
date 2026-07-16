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
}

// Not an APVTS parameter (it's a string, not automatable) - the path of an
// optional user-loaded impulse response file, persisted alongside the APVTS
// state as a plain XML attribute. See PluginProcessor::getStateInformation/
// setStateInformation and ReverbEngine::loadUserImpulseResponse.
namespace StateKeys
{
    inline constexpr auto userIrPath = "userIrPath";
}
