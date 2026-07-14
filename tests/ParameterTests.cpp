#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    // Convenience wrapper: fetches a parameter by ID and requires it to
    // exist before returning, so every SECTION below fails loudly (not with
    // a null-deref) if an ID typo ever creeps in.
    juce::RangedAudioParameter* requireParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        auto* param = apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param;
    }

    // Checks that a float parameter's underlying NormalisableRange covers
    // [expectedMin, expectedMax], independent of any skew/log mapping.
    void checkFloatRange (juce::AudioProcessorValueTreeState& apvts,
                           const juce::String& id,
                           float expectedMin,
                           float expectedMax)
    {
        auto* param = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (id));
        REQUIRE (param != nullptr);

        const auto range = param->getNormalisableRange().getRange();
        CHECK (range.getStart() == Catch::Approx (expectedMin));
        CHECK (range.getEnd() == Catch::Approx (expectedMax));
    }

    // Checks a float parameter's default value in real (non-normalised)
    // units, going through convertTo0to1 so log/skewed ranges are handled
    // the same way as linear ones.
    void checkFloatDefault (juce::AudioProcessorValueTreeState& apvts,
                             const juce::String& id,
                             float expectedDefault)
    {
        auto* param = requireParam (apvts, id);
        CHECK (param->getDefaultValue() == Catch::Approx (param->convertTo0to1 (expectedDefault)).margin (1e-4));
    }
}

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    RequiemAudioProcessor processor;
    auto& apvts = processor.apvts;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Requiem"));
    }

    SECTION ("all documented parameter IDs resolve")
    {
        static constexpr const char* allIds[] = {
            ParamIDs::decay, ParamIDs::preDelay, ParamIDs::damping, ParamIDs::width, ParamIDs::mix, ParamIDs::output,
            ParamIDs::space, ParamIDs::earlyLateBalance, ParamIDs::modulation, ParamIDs::freeze,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the v0.1.0 layout")
    {
        CHECK (apvts.processor.getParameters().size() == 10);
    }

    SECTION ("Decay: reverb time defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::decay, 2.5f);
        checkFloatRange (apvts, ParamIDs::decay, 0.1f, 10.0f);
    }

    SECTION ("Pre-Delay: defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::preDelay, 20.0f);
        checkFloatRange (apvts, ParamIDs::preDelay, 0.0f, 250.0f);
    }

    SECTION ("Damping: HF cutoff defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::damping, 8000.0f);
        checkFloatRange (apvts, ParamIDs::damping, 500.0f, 20000.0f);
    }

    SECTION ("Width: defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::width, 100.0f);
        checkFloatRange (apvts, ParamIDs::width, 0.0f, 200.0f);
    }

    SECTION ("Mix: dry/wet defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::mix, 35.0f);
        checkFloatRange (apvts, ParamIDs::mix, 0.0f, 100.0f);
    }

    SECTION ("Output: trim defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::output, 0.0f);
        checkFloatRange (apvts, ParamIDs::output, -24.0f, 24.0f);
    }

    SECTION ("Space: choice parameter with Cathedral/Hall/Chamber, defaulting to Hall")
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::space));
        REQUIRE (param != nullptr);

        CHECK (param->choices.size() == 3);
        CHECK (param->choices[0] == "Cathedral");
        CHECK (param->choices[1] == "Hall");
        CHECK (param->choices[2] == "Chamber");
        CHECK (param->getIndex() == 1); // default: Hall
    }

    SECTION ("Early/Late Balance: defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::earlyLateBalance, 80.0f);
        checkFloatRange (apvts, ParamIDs::earlyLateBalance, 0.0f, 100.0f);
    }

    SECTION ("Modulation: defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::modulation, 0.0f);
        checkFloatRange (apvts, ParamIDs::modulation, 0.0f, 100.0f);
    }

    SECTION ("Freeze: boolean parameter, off by default")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::freeze));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }
}
