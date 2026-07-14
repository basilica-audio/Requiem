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
