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

        //======================================================================
        // v0.3.0 "Living Tail" additions - appended after the v0.2.0
        // parameters, never inserted between them (ParameterIds.h "FROZEN"
        // note). Every default below is neutral: either a hard-bypass value
        // (Low Cut 20 Hz / High Cut 20 kHz / Duck 0%) or a value that is
        // structurally inaudible because Engine defaults to Classic
        // Convolution - the bit-identical v0.2.0 engine. A v0.2.0 session
        // reloaded into v0.3.0 therefore renders identically; see
        // tests/StateTests.cpp's same-binary migration render-null test.

        // Engine: topology selector. Order must match ReverbEngine::EngineMode.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::engineMode, 1 },
            "Engine",
            juce::StringArray { "Classic Convolution", "Hybrid Tail", "Tail Bloom" },
            0));

        // Tail Mod Mode: FDN tail-modulation topology. Order must match
        // FdnTail::ModulationMode. Matrix is pitch-stable by construction
        // (time-varying orthogonal feedback matrices); Lush deliberately
        // detunes (interpolated modulated delay reads).
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::tailModMode, 1 },
            "Tail Mod Mode",
            juce::StringArray { "Matrix", "Lush", "Off" },
            0));

        // Tail Mod Depth: 0-100%, default 40% (gated - only audible in the
        // two FDN engine modes).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::tailModDepth, 1 },
            "Tail Mod Depth",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            40.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Tail Mod Rate: 25-400% of the nominal randomised LFO base rates,
        // skewed so 100% (the nominal rates) sits at the knob's centre.
        {
            juce::NormalisableRange<float> tailModRateRange (25.0f, 400.0f, 0.1f);
            tailModRateRange.setSkewForCentre (100.0f);

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamIDs::tailModRate, 1 },
                "Tail Mod Rate",
                tailModRateRange,
                100.0f,
                juce::AudioParameterFloatAttributes().withLabel ("%")));
        }

        // Bloom: level of the FDN bloom branch in Tail Bloom mode, 0-100%,
        // default 30% (gated - inaudible in the other two modes).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::bloomAmount, 1 },
            "Bloom",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            30.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Low Cut: wet-path post-EQ high-pass, 20-2000 Hz. The 20 Hz
        // default is a *hard bypass* (the filter is not run at all), so the
        // wet path stays bit-identical to v0.2.0 until the user moves it.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::lowCut, 1 },
            "Low Cut",
            makeLogFrequencyRange (20.0f, 2000.0f),
            20.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        // High Cut: wet-path post-EQ low-pass, 1000-20000 Hz. The 20 kHz
        // default is a hard bypass, as above.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::highCut, 1 },
            "High Cut",
            makeLogFrequencyRange (1000.0f, 20000.0f),
            20000.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        // Duck: how far the (dry) input ducks the wet signal. 0% is a
        // bit-identical bypass - the wet gain is exactly 1.0.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::duckAmount, 1 },
            "Duck",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Duck Attack: 1-200 ms, skew centre 20 ms. Inert while Duck is 0%.
        {
            juce::NormalisableRange<float> duckAttackRange (1.0f, 200.0f, 0.1f);
            duckAttackRange.setSkewForCentre (20.0f);

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamIDs::duckAttack, 1 },
                "Duck Attack",
                duckAttackRange,
                10.0f,
                juce::AudioParameterFloatAttributes().withLabel ("ms")));
        }

        // Duck Release: 50-2000 ms, skew centre 300 ms. Inert while Duck is 0%.
        {
            juce::NormalisableRange<float> duckReleaseRange (50.0f, 2000.0f, 0.1f);
            duckReleaseRange.setSkewForCentre (300.0f);

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamIDs::duckRelease, 1 },
                "Duck Release",
                duckReleaseRange,
                250.0f,
                juce::AudioParameterFloatAttributes().withLabel ("ms")));
        }

        return layout;
    }
}
