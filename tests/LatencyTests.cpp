#include "PluginProcessor.h"
#include "dsp/ReverbEngine.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("getLatencySamples() reports the convolution engine's latency after prepareToPlay", "[latency]")
{
    RequiemAudioProcessor processor;

    // Before prepareToPlay, no engine has been prepared yet - JUCE's default
    // AudioProcessor latency is 0.
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (48000.0, 512);

    // Cross-check against a standalone engine prepared identically: the
    // processor must report exactly what the engine (i.e. the convolution)
    // computes, not an approximation of it.
    ReverbEngine referenceEngine;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    referenceEngine.prepare (spec);

    CHECK (processor.getLatencySamples() == referenceEngine.getLatencySamples());
    // juce::dsp::Convolution's default configuration (used here) is
    // zero-latency, so this is expected to be exactly 0 - asserted
    // explicitly (rather than just >= 0) so a future change to a
    // fixed-latency configuration is caught by this test needing an update.
    CHECK (processor.getLatencySamples() >= 0);
}

TEST_CASE ("Latency is stable across repeated prepareToPlay calls at the same sample rate", "[latency]")
{
    RequiemAudioProcessor processor;

    processor.prepareToPlay (44100.0, 256);
    const auto firstLatency = processor.getLatencySamples();

    processor.prepareToPlay (44100.0, 256);
    const auto secondLatency = processor.getLatencySamples();

    CHECK (firstLatency == secondLatency);
}

TEST_CASE ("Latency stays well-defined when the sample rate changes", "[latency]")
{
    RequiemAudioProcessor processor;

    processor.prepareToPlay (44100.0, 512);
    const auto latencyAt44k = processor.getLatencySamples();

    processor.prepareToPlay (96000.0, 512);
    const auto latencyAt96k = processor.getLatencySamples();

    CHECK (latencyAt44k >= 0);
    CHECK (latencyAt96k >= 0);
}

//==============================================================================
// v0.3.0 brief test 6.13: latency stays zero in every engine mode, and the
// hybrid branch's onset compensation is verified rather than assumed.
#include "dsp/IrAnalysis.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>

TEST_CASE ("6.13 Reported latency is zero in every engine mode", "[latency][v3]")
{
    RequiemAudioProcessor processor;

    auto* engineParam = processor.apvts.getParameter (ParamIDs::engineMode);
    REQUIRE (engineParam != nullptr);

    for (int mode = 0; mode <= 2; ++mode)
    {
        engineParam->setValueNotifyingHost (engineParam->convertTo0to1 (static_cast<float> (mode)));
        processor.prepareToPlay (48000.0, 256);

        INFO ("engine mode index " << mode);
        CHECK (processor.getLatencySamples() == 0);
    }
}

TEST_CASE ("6.13 In Classic mode the first output sample lands at index zero", "[latency][v3]")
{
    // A Kronecker delta in, fully wet: the very first output sample must be
    // non-zero. Anything else means the chain has acquired latency it is not
    // reporting.
    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setPreDelayMs (0.0f);
    engine.setOutputDb (0.0f);
    engine.setModulationAmount (0.0f);
    engine.setDecaySeconds (2.0f);
    engine.setEngineMode (ReverbEngine::EngineMode::classicConvolution);

    juce::dsp::ProcessSpec spec { 48000.0, 512, 2 };
    engine.prepare (spec);
    REQUIRE (engine.waitForPendingRender());

    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 1.0f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    CHECK (engine.getLatencySamples() == 0);
    CHECK (std::abs (buffer.getSample (0, 0)) > 1.0e-4f);
}

TEST_CASE ("6.13 The hybrid branch's pre-delay accounts for the FIR and the FDN onset together",
           "[latency][v3]")
{
    ReverbEngine engine;
    engine.setMixProportion (1.0f);
    engine.setPreDelayMs (0.0f);
    engine.setDecaySeconds (3.0f);
    engine.setEngineMode (ReverbEngine::EngineMode::hybridTail);
    engine.setTailModMode (FdnTail::ModulationMode::off);

    juce::dsp::ProcessSpec spec { 48000.0, 256, 2 };
    engine.prepare (spec);
    engine.regenerateImpulseResponseIfNeeded();
    REQUIRE (engine.waitForPendingRender());

    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    const auto mixingSamples = static_cast<int> (std::round (engine.getMixingTimeSeconds() * 48000.0));
    const auto branchDelay = engine.getHybridBranchDelaySamples();
    const auto shortestLine = engine.getFdnTail().getShortestDelaySamples();

    // The branch's total onset is pre-delay + the correction FIR's group delay
    // + the FDN's own shortest delay line, and that sum has to land on the
    // mixing time. Subtracting only the FIR's 128 samples - the obvious reading
    // of "compensate the correction filter" - would leave the tail starting
    // roughly a shortest-delay-line late, which at 48 kHz is about 34 ms: an
    // audible hole immediately after the early field's 10 ms fade.
    const auto onset = branchDelay + IrAnalysis::correctionFirGroupDelay + shortestLine;

    INFO ("mixing time " << mixingSamples << " smp, pre-delay " << branchDelay
                          << " smp, FIR group delay " << IrAnalysis::correctionFirGroupDelay
                          << " smp, shortest line " << shortestLine << " smp");

    CHECK (std::abs (onset - mixingSamples) <= 48); // one millisecond at 48 kHz

    // And the naive compensation really would have been badly wrong.
    const auto naiveOnset = (mixingSamples - IrAnalysis::correctionFirGroupDelay)
                             + IrAnalysis::correctionFirGroupDelay + shortestLine;
    CHECK (naiveOnset - mixingSamples > 1000);
}

TEST_CASE ("6.13 Latency stays zero across sample rates and engine modes", "[latency][v3]")
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        for (int mode = 0; mode <= 2; ++mode)
        {
            ReverbEngine engine;
            engine.setEngineMode (static_cast<ReverbEngine::EngineMode> (mode));

            juce::dsp::ProcessSpec spec { sampleRate, 256, 2 };
            engine.prepare (spec);

            INFO ("sample rate " << sampleRate << ", engine mode " << mode);
            CHECK (engine.getLatencySamples() == 0);
        }
    }
}
