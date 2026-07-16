#include "ParameterLayout.h"
#include "ParameterIds.h"

namespace
{
    // True logarithmic (base-10) mapping for frequency parameters, so slider/
    // knob travel spends equal space per octave rather than per Hz. Uses
    // juce::mapToLog10/mapFromLog10 rather than NormalisableRange's built-in
    // power-law skew, which only approximates a log curve.
    juce::NormalisableRange<float> makeLogFrequencyRange (float minHz, float maxHz)
    {
        return juce::NormalisableRange<float> (
            minHz,
            maxHz,
            [] (float rangeStart, float rangeEnd, float normalised)
            { return juce::mapToLog10 (normalised, rangeStart, rangeEnd); },
            [] (float rangeStart, float rangeEnd, float value)
            { return juce::mapFromLog10 (value, rangeStart, rangeEnd); });
    }
}

namespace rqm
{
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        //======================================================================
        // Decay: reverb time in seconds, 0.1-10 s, default 2.5 s. Skewed so
        // the perceptually useful 0.5-4 s range gets most of the knob travel.
        {
            juce::NormalisableRange<float> decayRange (0.1f, 10.0f, 0.01f);
            decayRange.setSkewForCentre (2.0f);

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamIDs::decay, 1 },
                "Decay",
                decayRange,
                2.5f,
                juce::AudioParameterFloatAttributes().withLabel ("s")));
        }

        //======================================================================
        // Pre-Delay: gap between the direct sound and the wet tail's onset,
        // 0-250 ms, default 20 ms.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::preDelay, 1 },
            "Pre-Delay",
            juce::NormalisableRange<float> (0.0f, 250.0f, 0.1f),
            20.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        //======================================================================
        // Damping: HF low-pass cutoff applied to the procedural IR's tail,
        // 500-20000 Hz, default 8000 Hz. Higher = brighter/less damped.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::damping, 1 },
            "Damping",
            makeLogFrequencyRange (500.0f, 20000.0f),
            8000.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        //======================================================================
        // Width: stereo width of the wet signal only, 0-200%, default 100%
        // (the convolution engine's natural, unmodified stereo image).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::width, 1 },
            "Width",
            juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f),
            100.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Mix: dry/wet. Default 35% - a cinematic reverb is normally blended
        // in, not run fully wet.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::mix, 1 },
            "Mix",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            35.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Output: trim applied after the dry/wet mix.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::output, 1 },
            "Output",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        //======================================================================
        // Space: shapes the early-reflection layer of the procedurally
        // generated impulse response. Default index 1 = Hall (the balanced
        // v0.1-era character). Order must match ReverbIR::SpaceType.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::space, 1 },
            "Space",
            juce::StringArray { "Cathedral", "Hall", "Chamber" },
            1));

        //======================================================================
        // Early/Late Balance: 0% = early-reflection layer dominant (short,
        // direct, distinct slap), 100% = diffuse late tail dominant (smooth
        // wash, no distinct early reflections). Default 80% keeps the tail
        // close to the v0.1-era all-diffuse character while still giving
        // the early layer some presence.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::earlyLateBalance, 1 },
            "Early/Late Balance",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            80.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Modulation: depth of a subtle post-convolution chorus-style
        // modulation applied to the wet tail only. Default 0% is a bit-
        // identical passthrough of the unmodulated tail.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::modulation, 1 },
            "Modulation",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Freeze: sustains the current tail's spectral content instead of
        // letting it decay. Off by default.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::freeze, 1 },
            "Freeze",
            false));

        //======================================================================
        // v0.2.0 additions (see docs/design-brief.md) - appended after the
        // v0.1.0/M1 parameters, never inserted between them (ParameterIds.h
        // "FROZEN" note).

        // Size: apparent size of the space, decoupled from Decay (RT60) and
        // Space (reflection character). Default 50%.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::size, 1 },
            "Size",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            50.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Bass Decay: RT60 multiplier for the low band only, 25-175%,
        // default 130% (bass rings measurably longer than mid/high).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::bassDecay, 1 },
            "Bass Decay",
            juce::NormalisableRange<float> (25.0f, 175.0f, 0.1f),
            130.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        return layout;
    }
}
