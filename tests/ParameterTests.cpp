#include "PluginProcessor.h"
#include "dsp/WetChain.h"
#include "params/ParameterIds.h"

#include <iterator>

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

    // Checks that a choice parameter's default is the expected index.
    void checkChoiceDefault (juce::AudioProcessorValueTreeState& apvts,
                              const juce::String& id,
                              int expectedIndex)
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id));
        REQUIRE (param != nullptr);
        CHECK (param->getIndex() == expectedIndex);
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
            ParamIDs::size, ParamIDs::bassDecay,
            ParamIDs::engineMode, ParamIDs::tailModMode, ParamIDs::tailModDepth, ParamIDs::tailModRate,
            ParamIDs::bloomAmount, ParamIDs::lowCut, ParamIDs::highCut,
            ParamIDs::duckAmount, ParamIDs::duckAttack, ParamIDs::duckRelease,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the v0.3.0 layout (v0.2.0's 12 plus the ten Living Tail additions)")
    {
        CHECK (apvts.processor.getParameters().size() == 22);
    }

    SECTION ("v0.3.0 parameters were appended, never inserted - the frozen order is intact")
    {
        // Parameter IDs are frozen (ParameterIds.h) and so, in practice, is
        // their order: a host that automates by index rather than by ID would
        // otherwise silently re-point every automation lane in every saved
        // session. The twelve pre-v0.3.0 parameters must still occupy indices
        // 0-11, in exactly the order they shipped in.
        const char* const orderedIds[] = {
            ParamIDs::decay, ParamIDs::preDelay, ParamIDs::damping, ParamIDs::width,
            ParamIDs::mix, ParamIDs::output, ParamIDs::space, ParamIDs::earlyLateBalance,
            ParamIDs::modulation, ParamIDs::freeze, ParamIDs::size, ParamIDs::bassDecay,
            ParamIDs::engineMode, ParamIDs::tailModMode, ParamIDs::tailModDepth, ParamIDs::tailModRate,
            ParamIDs::bloomAmount, ParamIDs::lowCut, ParamIDs::highCut,
            ParamIDs::duckAmount, ParamIDs::duckAttack, ParamIDs::duckRelease,
        };

        const auto& parameters = apvts.processor.getParameters();
        REQUIRE (parameters.size() == static_cast<int> (std::size (orderedIds)));

        for (int i = 0; i < parameters.size(); ++i)
        {
            const auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameters[i]);
            REQUIRE (withId != nullptr);
            INFO ("parameter index " << i);
            CHECK (withId->paramID == juce::String (orderedIds[i]));
        }
    }

    SECTION ("every v0.3.0 parameter defaults to a neutral value")
    {
        // The migration guarantee in one place: a v0.2.0 session reloaded into
        // v0.3.0 gets these ten defaults, and every one of them is either a
        // hard-bypass setting or gated behind Engine = Classic Convolution.
        checkChoiceDefault (apvts, ParamIDs::engineMode, 0);   // Classic Convolution
        checkChoiceDefault (apvts, ParamIDs::tailModMode, 0);  // Matrix (gated: silent in Classic)
        checkFloatDefault (apvts, ParamIDs::tailModDepth, 40.0f);
        checkFloatDefault (apvts, ParamIDs::tailModRate, 100.0f);
        checkFloatDefault (apvts, ParamIDs::bloomAmount, 30.0f);
        checkFloatDefault (apvts, ParamIDs::lowCut, 20.0f);      // hard bypass
        checkFloatDefault (apvts, ParamIDs::highCut, 20000.0f);  // hard bypass
        checkFloatDefault (apvts, ParamIDs::duckAmount, 0.0f);   // bit-identical bypass
        checkFloatDefault (apvts, ParamIDs::duckAttack, 10.0f);
        checkFloatDefault (apvts, ParamIDs::duckRelease, 250.0f);

        checkFloatRange (apvts, ParamIDs::lowCut, 20.0f, 2000.0f);
        checkFloatRange (apvts, ParamIDs::highCut, 1000.0f, 20000.0f);
        checkFloatRange (apvts, ParamIDs::duckAmount, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::duckAttack, 1.0f, 200.0f);
        checkFloatRange (apvts, ParamIDs::duckRelease, 50.0f, 2000.0f);
        checkFloatRange (apvts, ParamIDs::tailModDepth, 0.0f, 100.0f);
        checkFloatRange (apvts, ParamIDs::tailModRate, 25.0f, 400.0f);
        checkFloatRange (apvts, ParamIDs::bloomAmount, 0.0f, 100.0f);

        // The hard-bypass ends must match what WetChain treats as bypassed,
        // or the "default is bit-identical" guarantee silently stops holding.
        CHECK (apvts.getParameter (ParamIDs::lowCut)->convertFrom0to1 (
                   apvts.getParameter (ParamIDs::lowCut)->getDefaultValue())
               == Catch::Approx (WetChain::lowCutBypassHz));
        CHECK (apvts.getParameter (ParamIDs::highCut)->convertFrom0to1 (
                   apvts.getParameter (ParamIDs::highCut)->getDefaultValue())
               == Catch::Approx (WetChain::highCutBypassHz));
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

    SECTION ("Size: defaults and range (v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::size, 50.0f);
        checkFloatRange (apvts, ParamIDs::size, 0.0f, 100.0f);
    }

    SECTION ("Bass Decay: defaults and range (v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::bassDecay, 130.0f);
        checkFloatRange (apvts, ParamIDs::bassDecay, 25.0f, 175.0f);
    }
}
