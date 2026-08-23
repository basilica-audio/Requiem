#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include "TestHelpers.h"

TEST_CASE ("diag user ir processor", "[.diag]")
{
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("requiem_diag_ir2.wav");

    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (48000.0)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));
        juce::AudioBuffer<float> ir (2, 256);
        ir.clear();
        ir.setSample (0, 0, 1.0f);
        ir.setSample (1, 0, 1.0f);
        writer->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());
    }

    std::printf ("A: construct + prepare\n");
    RequiemAudioProcessor processorA;
    processorA.prepareToPlay (48000.0, 512);

    std::printf ("B: load user ir\n");
    REQUIRE (processorA.loadUserImpulseResponseFile (file));

    std::printf ("C: set params\n");
    auto* sizeParam = processorA.apvts.getParameter (ParamIDs::size);
    sizeParam->setValueNotifyingHost (sizeParam->convertTo0to1 (0.0f));

    std::printf ("D: dispatch loop\n");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    std::printf ("E1: second processor\n");
    RequiemAudioProcessor processorB;
    processorB.prepareToPlay (48000.0, 512);
    REQUIRE (processorB.loadUserImpulseResponseFile (file));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    std::printf ("E2: fillWithSine\n");
    juce::AudioBuffer<float> buffer (2, 512);
    juce::AudioBuffer<float> bufferB (2, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 500.0, 0.5f);
    TestHelpers::fillWithSine (bufferB, 48000.0, 500.0, 0.5f);

    std::printf ("E3: processBlock\n");
    juce::MidiBuffer midi;
    processorA.processBlock (buffer, midi);
    processorB.processBlock (bufferB, midi);

    std::printf ("F: done\n");
    file.deleteFile();
}
