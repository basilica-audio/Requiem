#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    // Writes a short, valid stereo WAV file to a temp location and returns
    // it, for tests that need a real user-IR file to load. The caller is
    // responsible for deleting it afterwards.
    juce::File writeTestImpulseResponseFile()
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("requiem_test_ir_" + juce::String (juce::Random::getSystemRandom().nextInt64()) + ".wav");

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        REQUIRE (stream != nullptr);

        auto writer = wavFormat.createWriterFor (stream,
            juce::AudioFormatWriterOptions()
                .withSampleRate (48000.0)
                .withNumChannels (2)
                .withBitsPerSample (16));
        REQUIRE (writer != nullptr);

        juce::AudioBuffer<float> irBuffer (2, 256);
        irBuffer.clear();
        irBuffer.setSample (0, 0, 1.0f);
        irBuffer.setSample (1, 0, 1.0f);

        writer->writeFromAudioSampleBuffer (irBuffer, 0, irBuffer.getNumSamples());
        writer.reset(); // flush/close before the caller reads the file back

        return file;
    }
}

TEST_CASE ("State round-trip preserves non-default values of every parameter", "[state]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* decayParam = processor.apvts.getParameter (ParamIDs::decay);
    auto* preDelayParam = processor.apvts.getParameter (ParamIDs::preDelay);
    auto* dampingParam = processor.apvts.getParameter (ParamIDs::damping);
    auto* widthParam = processor.apvts.getParameter (ParamIDs::width);
    auto* mixParam = processor.apvts.getParameter (ParamIDs::mix);
    auto* outputParam = processor.apvts.getParameter (ParamIDs::output);
    auto* spaceParam = processor.apvts.getParameter (ParamIDs::space);
    auto* earlyLateBalanceParam = processor.apvts.getParameter (ParamIDs::earlyLateBalance);
    auto* modulationParam = processor.apvts.getParameter (ParamIDs::modulation);
    auto* freezeParam = processor.apvts.getParameter (ParamIDs::freeze);

    REQUIRE (decayParam != nullptr);
    REQUIRE (preDelayParam != nullptr);
    REQUIRE (dampingParam != nullptr);
    REQUIRE (widthParam != nullptr);
    REQUIRE (mixParam != nullptr);
    REQUIRE (outputParam != nullptr);
    REQUIRE (spaceParam != nullptr);
    REQUIRE (earlyLateBalanceParam != nullptr);
    REQUIRE (modulationParam != nullptr);
    REQUIRE (freezeParam != nullptr);

    decayParam->setValueNotifyingHost (decayParam->convertTo0to1 (6.0f));
    preDelayParam->setValueNotifyingHost (preDelayParam->convertTo0to1 (120.0f));
    dampingParam->setValueNotifyingHost (dampingParam->convertTo0to1 (3500.0f));
    widthParam->setValueNotifyingHost (widthParam->convertTo0to1 (180.0f));
    mixParam->setValueNotifyingHost (mixParam->convertTo0to1 (70.0f));
    outputParam->setValueNotifyingHost (outputParam->convertTo0to1 (-4.5f));
    spaceParam->setValueNotifyingHost (spaceParam->convertTo0to1 (0.0f)); // Cathedral (non-default)
    earlyLateBalanceParam->setValueNotifyingHost (earlyLateBalanceParam->convertTo0to1 (25.0f));
    modulationParam->setValueNotifyingHost (modulationParam->convertTo0to1 (60.0f));
    freezeParam->setValueNotifyingHost (freezeParam->convertTo0to1 (1.0f)); // on (non-default)

    const auto savedDecay = decayParam->getValue();
    const auto savedPreDelay = preDelayParam->getValue();
    const auto savedDamping = dampingParam->getValue();
    const auto savedWidth = widthParam->getValue();
    const auto savedMix = mixParam->getValue();
    const auto savedOutput = outputParam->getValue();
    const auto savedSpace = spaceParam->getValue();
    const auto savedEarlyLateBalance = earlyLateBalanceParam->getValue();
    const auto savedModulation = modulationParam->getValue();
    const auto savedFreeze = freezeParam->getValue();

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    REQUIRE (savedState.getSize() > 0);

    // Reset every parameter back to its default before restoring, so the
    // round-trip assertion below can't pass by accident.
    decayParam->setValueNotifyingHost (decayParam->getDefaultValue());
    preDelayParam->setValueNotifyingHost (preDelayParam->getDefaultValue());
    dampingParam->setValueNotifyingHost (dampingParam->getDefaultValue());
    widthParam->setValueNotifyingHost (widthParam->getDefaultValue());
    mixParam->setValueNotifyingHost (mixParam->getDefaultValue());
    outputParam->setValueNotifyingHost (outputParam->getDefaultValue());
    spaceParam->setValueNotifyingHost (spaceParam->getDefaultValue());
    earlyLateBalanceParam->setValueNotifyingHost (earlyLateBalanceParam->getDefaultValue());
    modulationParam->setValueNotifyingHost (modulationParam->getDefaultValue());
    freezeParam->setValueNotifyingHost (freezeParam->getDefaultValue());

    REQUIRE (decayParam->getValue() != Catch::Approx (savedDecay));
    REQUIRE (preDelayParam->getValue() != Catch::Approx (savedPreDelay));
    REQUIRE (dampingParam->getValue() != Catch::Approx (savedDamping));
    REQUIRE (widthParam->getValue() != Catch::Approx (savedWidth));
    REQUIRE (mixParam->getValue() != Catch::Approx (savedMix));
    REQUIRE (outputParam->getValue() != Catch::Approx (savedOutput));
    REQUIRE (spaceParam->getValue() != Catch::Approx (savedSpace));
    REQUIRE (earlyLateBalanceParam->getValue() != Catch::Approx (savedEarlyLateBalance));
    REQUIRE (modulationParam->getValue() != Catch::Approx (savedModulation));
    REQUIRE (freezeParam->getValue() != Catch::Approx (savedFreeze));

    processor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    CHECK (decayParam->getValue() == Catch::Approx (savedDecay).margin (1e-6));
    CHECK (preDelayParam->getValue() == Catch::Approx (savedPreDelay).margin (1e-6));
    CHECK (dampingParam->getValue() == Catch::Approx (savedDamping).margin (1e-6));
    CHECK (widthParam->getValue() == Catch::Approx (savedWidth).margin (1e-6));
    CHECK (mixParam->getValue() == Catch::Approx (savedMix).margin (1e-6));
    CHECK (outputParam->getValue() == Catch::Approx (savedOutput).margin (1e-6));
    CHECK (spaceParam->getValue() == Catch::Approx (savedSpace).margin (1e-6));
    CHECK (earlyLateBalanceParam->getValue() == Catch::Approx (savedEarlyLateBalance).margin (1e-6));
    CHECK (modulationParam->getValue() == Catch::Approx (savedModulation).margin (1e-6));
    CHECK (freezeParam->getValue() == Catch::Approx (savedFreeze).margin (1e-6));
}

TEST_CASE ("State round-trip preserves an active user impulse-response file path", "[state]")
{
    const auto irFile = writeTestImpulseResponseFile();

    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    REQUIRE (processor.loadUserImpulseResponseFile (irFile));
    REQUIRE (processor.isUsingUserImpulseResponse());

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    REQUIRE (savedState.getSize() > 0);

    // A fresh processor instance, as a host would create when reloading a
    // saved session.
    RequiemAudioProcessor restoredProcessor;
    restoredProcessor.prepareToPlay (48000.0, 512);
    restoredProcessor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    CHECK (restoredProcessor.isUsingUserImpulseResponse());
    CHECK (restoredProcessor.getUserImpulseResponseFile() == irFile);

    irFile.deleteFile();
}

TEST_CASE ("State round-trip falls back to procedural when no user IR is active", "[state]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    REQUIRE_FALSE (processor.isUsingUserImpulseResponse());

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);

    RequiemAudioProcessor restoredProcessor;
    restoredProcessor.prepareToPlay (48000.0, 512);
    restoredProcessor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    CHECK_FALSE (restoredProcessor.isUsingUserImpulseResponse());
}

TEST_CASE ("Restoring a state whose user IR file no longer exists falls back to procedural", "[state]")
{
    const auto irFile = writeTestImpulseResponseFile();

    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    REQUIRE (processor.loadUserImpulseResponseFile (irFile));

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);

    irFile.deleteFile(); // simulate the IR file having moved/been deleted

    RequiemAudioProcessor restoredProcessor;
    restoredProcessor.prepareToPlay (48000.0, 512);
    CHECK_NOTHROW (restoredProcessor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize())));

    CHECK_FALSE (restoredProcessor.isUsingUserImpulseResponse());
}

TEST_CASE ("Robust user-IR loading: a non-audio file is rejected without changing state", "[state][robustness]")
{
    // A file that exists and has a .wav extension, but whose contents are
    // not a valid audio stream - a format-reader sanity check must reject
    // it before it ever reaches juce::dsp::Convolution.
    auto bogusFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("requiem_test_bogus_" + juce::String (juce::Random::getSystemRandom().nextInt64()) + ".wav");

    bogusFile.replaceWithText ("this is not a wav file, just plain text pretending to be one");

    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    REQUIRE_FALSE (processor.loadUserImpulseResponseFile (bogusFile));
    CHECK_FALSE (processor.isUsingUserImpulseResponse());

    // Processing must still work fine afterwards (procedural fallback
    // untouched by the rejected load attempt).
    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillWithSine (buffer, 48000.0, 500.0, 0.5f);
    juce::MidiBuffer midi;
    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));

    bogusFile.deleteFile();
}

TEST_CASE ("Robust user-IR loading: a nonexistent file is rejected without changing state", "[state][robustness]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const juce::File missingFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                        .getChildFile ("requiem_test_does_not_exist_" + juce::String (juce::Random::getSystemRandom().nextInt64()) + ".wav");

    REQUIRE_FALSE (missingFile.existsAsFile());
    CHECK_FALSE (processor.loadUserImpulseResponseFile (missingFile));
    CHECK_FALSE (processor.isUsingUserImpulseResponse());
}

TEST_CASE ("Robust user-IR loading: a file longer than maxUserImpulseResponseSeconds is rejected", "[state][robustness]")
{
    // A low sample rate keeps this file (and the test) fast while still
    // exceeding ReverbEngine::maxUserImpulseResponseSeconds in duration.
    constexpr double fileSampleRate = 4000.0;
    constexpr double fileDurationSeconds = ReverbEngine::maxUserImpulseResponseSeconds + 5.0;
    const auto numSamples = static_cast<int> (fileSampleRate * fileDurationSeconds);

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("requiem_test_too_long_" + juce::String (juce::Random::getSystemRandom().nextInt64()) + ".wav");

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    REQUIRE (stream != nullptr);

    auto writer = wavFormat.createWriterFor (stream,
        juce::AudioFormatWriterOptions()
            .withSampleRate (fileSampleRate)
            .withNumChannels (1)
            .withBitsPerSample (16));
    REQUIRE (writer != nullptr);

    juce::AudioBuffer<float> silence (1, numSamples);
    silence.clear();
    writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());
    writer.reset();

    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    CHECK_FALSE (processor.loadUserImpulseResponseFile (file));
    CHECK_FALSE (processor.isUsingUserImpulseResponse());

    file.deleteFile();
}

//==============================================================================
// v0.2.0 guarantee #9 (docs/design-brief.md): tolerant v1 -> v2 state
// import - unknown/removed v1 param IDs ignored (not applicable here, v1
// had no extra IDs v2 removed), size/bassDecay default to their v2 defaults
// when absent from a loaded v1 state, consistent with the
// AudioProcessorValueTreeState tolerant-load behaviour already relied on
// elsewhere in the suite.
TEST_CASE ("Tolerant v1->v2 state import: missing Size/Bass Decay default to their v2 defaults", "[state][v2]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // Perturb Decay away from its default so the "rest of the v1 state
    // still loads correctly" half of this test is meaningful too.
    auto* decayParam = processor.apvts.getParameter (ParamIDs::decay);
    REQUIRE (decayParam != nullptr);
    decayParam->setValueNotifyingHost (decayParam->convertTo0to1 (6.0f));

    const auto state = processor.apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    REQUIRE (xml != nullptr);

    // Simulate a genuine v0.1.x-era saved state: APVTS persists each
    // parameter as a <PARAM id="..." value="..."/> child element - removing
    // Size/Bass Decay's elements reproduces exactly what a v1 session file
    // (which never had these two parameters) looks like.
    for (int i = xml->getNumChildElements() - 1; i >= 0; --i)
    {
        auto* child = xml->getChildElement (i);

        if (child->hasTagName ("PARAM"))
        {
            const auto id = child->getStringAttribute ("id");

            if (id == juce::String (ParamIDs::size) || id == juce::String (ParamIDs::bassDecay))
                xml->removeChildElement (child, true);
        }
    }

    juce::MemoryBlock v1StyleState;
    juce::AudioProcessor::copyXmlToBinary (*xml, v1StyleState);

    RequiemAudioProcessor restoredProcessor;
    restoredProcessor.prepareToPlay (48000.0, 512);

    CHECK_NOTHROW (restoredProcessor.setStateInformation (v1StyleState.getData(), static_cast<int> (v1StyleState.getSize())));

    auto* restoredDecay = restoredProcessor.apvts.getParameter (ParamIDs::decay);
    auto* restoredSize = restoredProcessor.apvts.getParameter (ParamIDs::size);
    auto* restoredBassDecay = restoredProcessor.apvts.getParameter (ParamIDs::bassDecay);
    REQUIRE (restoredDecay != nullptr);
    REQUIRE (restoredSize != nullptr);
    REQUIRE (restoredBassDecay != nullptr);

    // The rest of the v1 state still loaded correctly...
    CHECK (restoredDecay->convertFrom0to1 (restoredDecay->getValue()) == Catch::Approx (6.0f).margin (1.0e-3));

    // ...and the two new v2 parameters, absent from the v1 XML, resolved to
    // their ParameterLayout defaults rather than crashing or reading
    // garbage.
    CHECK (restoredSize->convertFrom0to1 (restoredSize->getValue()) == Catch::Approx (50.0f).margin (1.0e-3));
    CHECK (restoredBassDecay->convertFrom0to1 (restoredBassDecay->getValue()) == Catch::Approx (130.0f).margin (1.0e-3));
}

TEST_CASE ("User IR override unaffected: Size and Bass Decay have zero effect on output while a user IR is active",
           "[state][v2]")
{
    const auto irFile = writeTestImpulseResponseFile();

    RequiemAudioProcessor processorA;
    processorA.prepareToPlay (48000.0, 512);
    REQUIRE (processorA.loadUserImpulseResponseFile (irFile));

    RequiemAudioProcessor processorB;
    processorB.prepareToPlay (48000.0, 512);
    REQUIRE (processorB.loadUserImpulseResponseFile (irFile));

    auto* sizeParamA = processorA.apvts.getParameter (ParamIDs::size);
    auto* bassDecayParamA = processorA.apvts.getParameter (ParamIDs::bassDecay);
    REQUIRE (sizeParamA != nullptr);
    REQUIRE (bassDecayParamA != nullptr);
    sizeParamA->setValueNotifyingHost (sizeParamA->convertTo0to1 (0.0f));
    bassDecayParamA->setValueNotifyingHost (bassDecayParamA->convertTo0to1 (25.0f));

    auto* sizeParamB = processorB.apvts.getParameter (ParamIDs::size);
    auto* bassDecayParamB = processorB.apvts.getParameter (ParamIDs::bassDecay);
    REQUIRE (sizeParamB != nullptr);
    REQUIRE (bassDecayParamB != nullptr);
    sizeParamB->setValueNotifyingHost (sizeParamB->convertTo0to1 (100.0f));
    bassDecayParamB->setValueNotifyingHost (bassDecayParamB->convertTo0to1 (175.0f));

    // Let the message-thread timer actually run
    // (regenerateImpulseResponseIfNeeded() is a no-op while
    // usingUserImpulseResponse is true, per ReverbEngine's own gate) so
    // this test would fail if that gate were ever removed/bypassed for the
    // two new v0.2.0 parameters specifically.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    juce::AudioBuffer<float> bufferA (2, 512);
    juce::AudioBuffer<float> bufferB (2, 512);
    TestHelpers::fillWithSine (bufferA, 48000.0, 500.0, 0.5f);
    TestHelpers::fillWithSine (bufferB, 48000.0, 500.0, 0.5f);

    juce::MidiBuffer midi;
    processorA.processBlock (bufferA, midi);
    processorB.processBlock (bufferB, midi);

    for (int channel = 0; channel < bufferA.getNumChannels(); ++channel)
    {
        const auto* a = bufferA.getReadPointer (channel);
        const auto* b = bufferB.getReadPointer (channel);

        for (int i = 0; i < bufferA.getNumSamples(); ++i)
            CHECK (a[i] == Catch::Approx (b[i]).margin (1.0e-7));
    }

    irFile.deleteFile();
}
