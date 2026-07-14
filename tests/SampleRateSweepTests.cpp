// Sample-rate sweep coverage for the M1 test-coverage milestone: 44.1 kHz
// through 192 kHz, at both the engine level (null test replicated per rate)
// and the processor level (finite output, stable/well-defined latency).
#include "PluginProcessor.h"
#include "dsp/ReverbEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    // The full documented operating range (see docs/architecture.md /
    // README): 44.1-192 kHz, plus a couple of the more unusual rates hosts
    // sometimes use (88.2/176.4 kHz), all as one parametrised list so this
    // stays a single fast test rather than N near-duplicate ones.
    constexpr double sweepSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

    constexpr int sweepBlockSize = 512;
}

TEST_CASE ("Engine null test holds across the documented sample-rate range", "[dsp][engine][null][samplerate]")
{
    for (const auto sr : sweepSampleRates)
    {
        DYNAMIC_SECTION ("sample rate " << sr << " Hz")
        {
            ReverbEngine engine;
            engine.setMixProportion (0.0f);
            engine.setDecaySeconds (1.5f);
            engine.setDampingHz (4000.0f);
            engine.setPreDelayMs (30.0f);
            engine.setWidthPercent (140.0f);
            engine.setOutputDb (0.0f);

            juce::dsp::ProcessSpec spec;
            spec.sampleRate = sr;
            spec.maximumBlockSize = static_cast<juce::uint32> (sweepBlockSize);
            spec.numChannels = 2;
            engine.prepare (spec);

            const auto latency = engine.getLatencySamples();
            REQUIRE (latency >= 0);

            constexpr int numBlocks = 4;
            const auto totalSamples = sweepBlockSize * numBlocks;
            REQUIRE (latency < totalSamples / 2);

            juce::AudioBuffer<float> reference (2, totalSamples);
            TestHelpers::fillWithSine (reference, sr, 500.0, 0.5f);

            juce::AudioBuffer<float> processed;
            processed.makeCopyOf (reference);

            for (int b = 0; b < numBlocks; ++b)
            {
                juce::dsp::AudioBlock<float> block (processed);
                auto sub = block.getSubBlock (static_cast<size_t> (b * sweepBlockSize), static_cast<size_t> (sweepBlockSize));
                engine.process (sub);
            }

            CHECK (TestHelpers::allSamplesFinite (processed));

            const auto overlapLength = totalSamples - latency;
            REQUIRE (overlapLength > totalSamples / 2);

            constexpr float tolerance = 3.1623e-5f; // < -90 dBFS residual

            for (int channel = 0; channel < reference.getNumChannels(); ++channel)
            {
                const auto* refData = reference.getReadPointer (channel);
                const auto* outData = processed.getReadPointer (channel);

                float maxResidual = 0.0f;

                for (int i = 0; i < overlapLength; ++i)
                    maxResidual = std::max (maxResidual, std::abs (outData[latency + i] - refData[i]));

                CHECK (maxResidual < tolerance);
            }
        }
    }
}

TEST_CASE ("Processor produces finite output and well-defined latency across the documented sample-rate range", "[processor][samplerate]")
{
    for (const auto sr : sweepSampleRates)
    {
        DYNAMIC_SECTION ("sample rate " << sr << " Hz")
        {
            RequiemAudioProcessor processor;
            processor.prepareToPlay (sr, sweepBlockSize);

            CHECK (processor.getLatencySamples() >= 0);

            juce::AudioBuffer<float> buffer (2, sweepBlockSize);
            TestHelpers::fillWithSine (buffer, sr, 220.0, 0.6f);
            juce::MidiBuffer midi;

            for (int i = 0; i < 4; ++i)
                CHECK_NOTHROW (processor.processBlock (buffer, midi));

            CHECK (TestHelpers::allSamplesFinite (buffer));
        }
    }
}

TEST_CASE ("Repeated sample-rate changes across the full sweep never leave stale/undefined state", "[processor][samplerate]")
{
    RequiemAudioProcessor processor;

    for (const auto sr : sweepSampleRates)
    {
        processor.prepareToPlay (sr, sweepBlockSize);

        juce::AudioBuffer<float> buffer (2, sweepBlockSize);
        TestHelpers::fillWithSine (buffer, sr, 330.0, 0.5f);
        juce::MidiBuffer midi;

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }

    // ...and back down again, in reverse, to also exercise shrinking rates.
    for (auto it = std::rbegin (sweepSampleRates); it != std::rend (sweepSampleRates); ++it)
    {
        processor.prepareToPlay (*it, sweepBlockSize);

        juce::AudioBuffer<float> buffer (2, sweepBlockSize);
        TestHelpers::fillWithSine (buffer, *it, 330.0, 0.5f);
        juce::MidiBuffer midi;

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}
