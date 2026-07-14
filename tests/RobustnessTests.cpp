#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <random>

namespace
{
    void setParam (RequiemAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }
}

TEST_CASE ("Silence produces silence (and no NaN/Inf)", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::mix, 100.0f);
    setParam (processor, ParamIDs::decay, 5.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Full-scale input at maximum decay/width/output produces no NaN/Inf", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::decay, 10.0f);
    setParam (processor, ParamIDs::preDelay, 250.0f);
    setParam (processor, ParamIDs::damping, 20000.0f);
    setParam (processor, ParamIDs::width, 200.0f);
    setParam (processor, ParamIDs::mix, 100.0f);
    setParam (processor, ParamIDs::output, 24.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 1000.0, 1.0f);

    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (TestHelpers::peakAbsolute (buffer) < 1000.0f); // sane bound, not just "finite"
}

TEST_CASE ("Denormal-range input produces no NaN/Inf output", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::mix, 100.0f);

    constexpr int numSamples = 512;
    juce::AudioBuffer<float> buffer (2, numSamples);

    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash processBlock", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (buffer.getNumSamples() == 0);
}

TEST_CASE ("Extreme parameter values at both range edges produce no NaN/Inf", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (44100.0, 256);

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;

    for (bool useMinimum : { true, false })
    {
        setParam (processor, ParamIDs::decay, useMinimum ? 0.1f : 10.0f);
        setParam (processor, ParamIDs::preDelay, useMinimum ? 0.0f : 250.0f);
        setParam (processor, ParamIDs::damping, useMinimum ? 500.0f : 20000.0f);
        setParam (processor, ParamIDs::width, useMinimum ? 0.0f : 200.0f);
        setParam (processor, ParamIDs::mix, useMinimum ? 0.0f : 100.0f);
        setParam (processor, ParamIDs::output, useMinimum ? -24.0f : 24.0f);
        setParam (processor, ParamIDs::space, useMinimum ? 0.0f : 2.0f); // Cathedral / Chamber
        setParam (processor, ParamIDs::earlyLateBalance, useMinimum ? 0.0f : 100.0f);
        setParam (processor, ParamIDs::modulation, useMinimum ? 0.0f : 100.0f);
        setParam (processor, ParamIDs::freeze, useMinimum ? 0.0f : 1.0f);

        TestHelpers::fillWithSine (buffer, 44100.0, 440.0, 0.8f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Rapid parameter automation across many blocks produces no NaN/Inf", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    std::mt19937 rng (1234);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    juce::MidiBuffer midi;

    for (int block = 0; block < 100; ++block)
    {
        setParam (processor, ParamIDs::decay, 0.1f + unit (rng) * 9.9f);
        setParam (processor, ParamIDs::preDelay, unit (rng) * 250.0f);
        setParam (processor, ParamIDs::damping, 500.0f + unit (rng) * 19500.0f);
        setParam (processor, ParamIDs::width, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::mix, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::output, -24.0f + unit (rng) * 48.0f);

        juce::AudioBuffer<float> buffer (2, 256);
        TestHelpers::fillWithSine (buffer, 48000.0, 200.0 + unit (rng) * 4000.0, 0.7f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("reset() followed by processBlock does not crash", "[robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::decay, 3.0f);
    setParam (processor, ParamIDs::mix, 100.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 1000.0, 0.6f);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK_NOTHROW (processor.reset());

    TestHelpers::fillWithSine (buffer, 48000.0, 1000.0, 0.6f);
    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}
