// Bus-layout coverage for the M1 test-coverage milestone: mono and stereo
// configurations (the two layouts isBusesLayoutSupported() actually
// allows), plus explicit rejection checks for layouts it must refuse
// (mismatched in/out channel counts, and unsupported channel sets such as
// 5.1). Requiem does not add a sidechain input bus, so this deliberately
// does not attempt to test one.
#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    juce::AudioProcessor::BusesLayout makeLayout (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (in);
        layout.outputBuses.add (out);
        return layout;
    }
}

TEST_CASE ("isBusesLayoutSupported() accepts mono and stereo, in == out", "[processor][buses]")
{
    RequiemAudioProcessor processor;

    CHECK (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));
    CHECK (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));
}

TEST_CASE ("isBusesLayoutSupported() rejects mismatched in/out channel sets", "[processor][buses]")
{
    RequiemAudioProcessor processor;

    CHECK_FALSE (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo())));
    CHECK_FALSE (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono())));
}

TEST_CASE ("isBusesLayoutSupported() rejects unsupported channel sets", "[processor][buses]")
{
    RequiemAudioProcessor processor;

    CHECK_FALSE (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::create5point1())));
    CHECK_FALSE (processor.isBusesLayoutSupported (makeLayout (juce::AudioChannelSet::disabled(), juce::AudioChannelSet::disabled())));
}

TEST_CASE ("Mono processing produces finite output and a well-defined latency", "[processor][buses]")
{
    RequiemAudioProcessor processor;

    REQUIRE (processor.setBusesLayout (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));

    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() >= 0);

    juce::AudioBuffer<float> buffer (1, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 500.0, 0.6f);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Stereo processing produces finite output and a well-defined latency", "[processor][buses]")
{
    RequiemAudioProcessor processor;

    REQUIRE (processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));

    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() >= 0);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 500.0, 0.6f);
    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Switching bus layout between mono and stereo across prepareToPlay calls does not crash", "[processor][buses]")
{
    RequiemAudioProcessor processor;
    juce::MidiBuffer midi;

    REQUIRE (processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));
    processor.prepareToPlay (48000.0, 512);
    {
        juce::AudioBuffer<float> stereoBuffer (2, 512);
        TestHelpers::fillWithSine (stereoBuffer, 48000.0, 500.0, 0.5f);
        CHECK_NOTHROW (processor.processBlock (stereoBuffer, midi));
        CHECK (TestHelpers::allSamplesFinite (stereoBuffer));
    }

    REQUIRE (processor.setBusesLayout (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));
    processor.prepareToPlay (48000.0, 512);
    {
        juce::AudioBuffer<float> monoBuffer (1, 512);
        TestHelpers::fillWithSine (monoBuffer, 48000.0, 500.0, 0.5f);
        CHECK_NOTHROW (processor.processBlock (monoBuffer, midi));
        CHECK (TestHelpers::allSamplesFinite (monoBuffer));
    }

    REQUIRE (processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));
    processor.prepareToPlay (48000.0, 512);
    {
        juce::AudioBuffer<float> stereoBuffer (2, 512);
        TestHelpers::fillWithSine (stereoBuffer, 48000.0, 500.0, 0.5f);
        CHECK_NOTHROW (processor.processBlock (stereoBuffer, midi));
        CHECK (TestHelpers::allSamplesFinite (stereoBuffer));
    }
}
