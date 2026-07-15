// Long-run NaN/Inf stability coverage for the M1 test-coverage milestone:
// several seconds' worth of audio across many blocks, with every parameter
// (including the M1 additions - Space/Early-Late-Balance/Modulation/
// Freeze) under continuous randomised automation. Bounded to stay fast in
// Debug CI (a few thousand small blocks is trivial CPU, well under a
// minute even on a slow Windows Debug runner).
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE ("Long-run: thousands of small blocks under full parameter automation stay finite", "[robustness][longrun]")
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 128;
    // 2000 blocks * 128 samples / 48 kHz ~= 5.3 s of audio - a meaningful
    // long run without being slow to execute.
    constexpr int numBlocks = 2000;

    RequiemAudioProcessor processor;
    processor.prepareToPlay (sr, blockSize);

    std::mt19937 rng (9001);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);
    std::uniform_int_distribution<int> spaceIndex (0, 2);

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < numBlocks; ++block)
    {
        setParam (processor, ParamIDs::decay, 0.1f + unit (rng) * 9.9f);
        setParam (processor, ParamIDs::preDelay, unit (rng) * 250.0f);
        setParam (processor, ParamIDs::damping, 500.0f + unit (rng) * 19500.0f);
        setParam (processor, ParamIDs::width, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::mix, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::output, -24.0f + unit (rng) * 48.0f);
        setParam (processor, ParamIDs::space, static_cast<float> (spaceIndex (rng)));
        setParam (processor, ParamIDs::earlyLateBalance, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::modulation, unit (rng) * 100.0f);
        // Toggle Freeze occasionally, not every block - a realistic usage
        // pattern (a user flips the switch, doesn't automate it at audio
        // rate) that still exercises repeated on/off IR regeneration over
        // the course of the run.
        if (block % 137 == 0)
            setParam (processor, ParamIDs::freeze, unit (rng) < 0.5f ? 0.0f : 1.0f);

        TestHelpers::fillWithSine (buffer, sr, 80.0 + unit (rng) * 6000.0, 0.6f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));

        if (! TestHelpers::allSamplesFinite (buffer))
        {
            FAIL_CHECK ("Non-finite sample at block " << block);
            break;
        }

        // Periodically let the real message-thread Timer actually fire
        // (see issue #11 item 2: "the full-automation test's Space/Early-
        // Late-Balance/Freeze coverage" gap). Without this,
        // RequiemAudioProcessor's 20 Hz juce::Timer never dispatches in
        // this headless test harness, so none of the Space/Early-Late-
        // Balance/Freeze combinations set above ever actually get
        // regenerated/applied - every block in the run would silently keep
        // processing against whatever IR prepareToPlay() generated from
        // the defaults, and the "no NaN/Inf" assertions above would only
        // ever be exercising that one IR, not the randomised parameters
        // this test claims to cover.
        if (block % 100 == 99)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
    }
}

TEST_CASE ("Long-run: frozen tail processed continuously for several seconds stays finite and bounded", "[robustness][longrun]")
{
    constexpr double sr = 48000.0;
    constexpr int blockSize = 256;
    constexpr int numBlocks = 800; // ~4.3 s

    RequiemAudioProcessor processor;
    processor.prepareToPlay (sr, blockSize);

    setParam (processor, ParamIDs::freeze, 1.0f);
    setParam (processor, ParamIDs::mix, 100.0f);
    setParam (processor, ParamIDs::decay, 3.0f);
    setParam (processor, ParamIDs::modulation, 75.0f);

    // Let the real ~20 Hz message-thread Timer actually fire and drive
    // ReverbEngine::regenerateImpulseResponseIfNeeded() (see issue #11
    // item 1). Without this, the Freeze/Decay/Modulation values set above
    // only ever update atomics - the frozen-envelope IR this test is
    // supposed to be exercising is never actually generated (nor, per
    // issue #13's fix, handed to the audio thread to load), so the "frozen
    // tail" measured below would silently just be whatever IR
    // prepareToPlay() generated from the (non-frozen) defaults.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, blockSize);

    // A single impulse to seed the frozen tail, then silence - the frozen
    // pad should sustain (not blow up) for the remainder of the run.
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 1.0f);

    for (int block = 0; block < numBlocks; ++block)
    {
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
        CHECK (TestHelpers::peakAbsolute (buffer) < 1000.0f); // sane bound, not just "finite"

        buffer.clear();
    }
}
