#include "dsp/WetChain.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

// v0.3.0 brief test 6.11: the wet-path low cut, high cut and ducker.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 256;

    juce::dsp::ProcessSpec specFor (double sampleRate, int blockSize = testBlockSize)
    {
        return { sampleRate, static_cast<juce::uint32> (blockSize), 2 };
    }

    // Runs `numSamples` of a signal produced by `generator` through the chain,
    // returning the left channel. `sidechain` supplies the ducker's mono input;
    // when null the chain's own (post-filter) signal is not used at all - the
    // ducker simply does not run.
    std::vector<float> runChain (WetChain& chain, int numSamples,
                                  const std::function<float (int)>& generator,
                                  const std::function<float (int)>& sidechain = {})
    {
        std::vector<float> output (static_cast<size_t> (numSamples), 0.0f);
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        std::vector<float> mono (static_cast<size_t> (testBlockSize), 0.0f);

        auto written = 0;

        while (written < numSamples)
        {
            const auto thisBlock = juce::jmin (testBlockSize, numSamples - written);

            for (int i = 0; i < thisBlock; ++i)
            {
                const auto sample = generator (written + i);
                buffer.setSample (0, i, sample);
                buffer.setSample (1, i, sample);
                mono[static_cast<size_t> (i)] = sidechain ? sidechain (written + i) : 0.0f;
            }

            auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, static_cast<size_t> (thisBlock));
            chain.process (block, sidechain ? mono.data() : nullptr, thisBlock);

            for (int i = 0; i < thisBlock; ++i)
                output[static_cast<size_t> (written + i)] = buffer.getSample (0, i);

            written += thisBlock;
        }

        return output;
    }

    // Steady-state magnitude response, in dB, at one frequency: drives a sine
    // through a freshly reset chain, discards the settling transient, and
    // measures RMS against the input's.
    float responseDb (WetChain& chain, float frequencyHz)
    {
        chain.reset();

        const auto totalSamples = static_cast<int> (testSampleRate * 0.5);
        const auto skipSamples = static_cast<int> (testSampleRate * 0.2);

        const auto generator = [frequencyHz] (int n)
        {
            return 0.5f * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequencyHz
                                                          * static_cast<double> (n) / testSampleRate));
        };

        const auto rendered = runChain (chain, totalSamples, generator);

        double outputEnergy = 0.0;
        double inputEnergy = 0.0;

        for (int i = skipSamples; i < totalSamples; ++i)
        {
            const auto out = static_cast<double> (rendered[static_cast<size_t> (i)]);
            const auto in = static_cast<double> (generator (i));
            outputEnergy += out * out;
            inputEnergy += in * in;
        }

        if (inputEnergy <= 0.0 || outputEnergy <= 0.0)
            return -200.0f;

        return static_cast<float> (10.0 * std::log10 (outputEnergy / inputEnergy));
    }
}

//==============================================================================
TEST_CASE ("6.11 At its defaults the wet chain is a literal no-op", "[wetchain][null]")
{
    WetChain chain;
    chain.prepare (specFor (testSampleRate));

    REQUIRE (chain.isHardBypassed());

    juce::Random random (5);
    std::vector<float> input (4096);

    for (auto& sample : input)
        sample = random.nextFloat() * 2.0f - 1.0f;

    // A loud sidechain too: at Duck 0% it must make no difference whatsoever.
    const auto rendered = runChain (chain, static_cast<int> (input.size()),
                                     [&input] (int n) { return input[static_cast<size_t> (n)]; },
                                     [] (int) { return 0.9f; });

    for (size_t i = 0; i < input.size(); ++i)
    {
        INFO ("sample " << i);
        // Not "close to": exactly. The defaults skip the filters and the gain
        // multiply outright, which is what makes a v0.2.0 session reload
        // bit-identically even though the wet chain sits in the signal path.
        REQUIRE (rendered[i] == input[i]);
    }
}

TEST_CASE ("6.11 Low cut and high cut roll off at 12 dB per octave", "[wetchain][eq]")
{
    SECTION ("low cut")
    {
        WetChain chain;
        chain.prepare (specFor (testSampleRate));
        chain.setLowCutHz (500.0f);

        // Let the 20 ms cutoff smoother settle before measuring.
        runChain (chain, static_cast<int> (testSampleRate * 0.1), [] (int) { return 0.0f; });

        const auto atCutoff = responseDb (chain, 500.0f);
        const auto oneOctaveBelow = responseDb (chain, 250.0f);
        const auto twoOctavesBelow = responseDb (chain, 125.0f);

        INFO ("at cutoff " << atCutoff << " dB, -1 oct " << oneOctaveBelow
                            << " dB, -2 oct " << twoOctavesBelow << " dB");

        // A second-order Butterworth high-pass is -3 dB at its corner and
        // asymptotes to 12 dB per octave below it.
        CHECK (std::abs (atCutoff - (-3.0f)) < 1.5f);
        CHECK (std::abs ((twoOctavesBelow - oneOctaveBelow) - (-12.0f)) < 1.5f);
    }

    SECTION ("high cut")
    {
        WetChain chain;
        chain.prepare (specFor (testSampleRate));
        chain.setHighCutHz (2000.0f);

        runChain (chain, static_cast<int> (testSampleRate * 0.1), [] (int) { return 0.0f; });

        const auto atCutoff = responseDb (chain, 2000.0f);
        const auto oneOctaveAbove = responseDb (chain, 4000.0f);
        const auto twoOctavesAbove = responseDb (chain, 8000.0f);

        INFO ("at cutoff " << atCutoff << " dB, +1 oct " << oneOctaveAbove
                            << " dB, +2 oct " << twoOctavesAbove << " dB");

        CHECK (std::abs (atCutoff - (-3.0f)) < 1.5f);
        CHECK (std::abs ((twoOctavesAbove - oneOctaveAbove) - (-12.0f)) < 1.5f);
    }
}

TEST_CASE ("6.11 The ducker's attack and release mean what they say", "[wetchain][duck]")
{
    constexpr float attackMs = 20.0f;
    constexpr float releaseMs = 200.0f;

    WetChain chain;
    chain.prepare (specFor (testSampleRate));
    chain.setDuckAmountPercent (100.0f);
    chain.setDuckAttackMs (attackMs);
    chain.setDuckReleaseMs (releaseMs);

    // A DC sidechain at exactly the reference envelope level, so the duck gain
    // tracks the envelope linearly rather than saturating at full depth part
    // way through the attack - which would make the measured time meaningless.
    const auto stepStart = static_cast<int> (testSampleRate * 0.1);
    const auto stepEnd = stepStart + static_cast<int> (testSampleRate * 0.5);
    const auto totalSamples = stepEnd + static_cast<int> (testSampleRate * 1.5);

    const auto rendered = runChain (chain, totalSamples,
                                     [] (int) { return 1.0f; },
                                     [stepStart, stepEnd] (int n)
                                     {
                                         return n >= stepStart && n < stepEnd ? WetChain::duckReferenceEnvelope : 0.0f;
                                     });

    // The wet signal is a constant 1.0, so the rendered output *is* the gain.
    const auto gainAt = [&rendered] (int n) { return rendered[static_cast<size_t> (n)]; };

    const auto floorGain = gainAt (stepEnd - 1);
    REQUIRE (floorGain < 0.2f);

    // Attack: the gain should have covered 63% of its excursion after one
    // attack time constant.
    const auto attackTargetGain = 1.0f - 0.63212f * (1.0f - floorGain);
    auto attackSamples = -1;

    for (int n = stepStart; n < stepEnd; ++n)
    {
        if (gainAt (n) <= attackTargetGain)
        {
            attackSamples = n - stepStart;
            break;
        }
    }

    REQUIRE (attackSamples > 0);
    const auto measuredAttackMs = 1000.0f * static_cast<float> (attackSamples) / static_cast<float> (testSampleRate);
    INFO ("measured attack " << measuredAttackMs << " ms against " << attackMs << " ms");
    CHECK (std::abs (measuredAttackMs / attackMs - 1.0f) < 0.20f);

    // Release: 63% of the way back to unity after one release time constant.
    const auto releaseTargetGain = floorGain + 0.63212f * (1.0f - floorGain);
    auto releaseSamples = -1;

    for (int n = stepEnd; n < totalSamples; ++n)
    {
        if (gainAt (n) >= releaseTargetGain)
        {
            releaseSamples = n - stepEnd;
            break;
        }
    }

    REQUIRE (releaseSamples > 0);
    const auto measuredReleaseMs = 1000.0f * static_cast<float> (releaseSamples) / static_cast<float> (testSampleRate);
    INFO ("measured release " << measuredReleaseMs << " ms against " << releaseMs << " ms");
    CHECK (std::abs (measuredReleaseMs / releaseMs - 1.0f) < 0.20f);
}

TEST_CASE ("6.11 Automating Duck across its whole range produces no zipper", "[wetchain][duck]")
{
    WetChain chain;
    chain.prepare (specFor (testSampleRate));
    chain.setDuckAttackMs (10.0f);
    chain.setDuckReleaseMs (250.0f);

    const auto totalSamples = static_cast<int> (testSampleRate * 2.0);
    std::vector<float> output (static_cast<size_t> (totalSamples), 0.0f);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> mono (static_cast<size_t> (testBlockSize), 0.0f);

    auto written = 0;
    juce::Random random (17);

    while (written < totalSamples)
    {
        const auto thisBlock = juce::jmin (testBlockSize, totalSamples - written);

        // A full-range Duck sweep, moved every block - the worst case a host's
        // automation lane can produce.
        const auto duck = 100.0f * static_cast<float> (written) / static_cast<float> (totalSamples);
        chain.setDuckAmountPercent (written % (2 * testBlockSize) < testBlockSize ? duck : 100.0f - duck);

        for (int i = 0; i < thisBlock; ++i)
        {
            // A constant wet signal, so any step in the output is a step in
            // the gain and nothing else.
            buffer.setSample (0, i, 1.0f);
            buffer.setSample (1, i, 1.0f);
            mono[static_cast<size_t> (i)] = 0.3f + 0.05f * (random.nextFloat() - 0.5f);
        }

        auto block = juce::dsp::AudioBlock<float> (buffer).getSubBlock (0, static_cast<size_t> (thisBlock));
        chain.process (block, mono.data(), thisBlock);

        for (int i = 0; i < thisBlock; ++i)
            output[static_cast<size_t> (written + i)] = buffer.getSample (0, i);

        written += thisBlock;
    }

    auto largestStep = 0.0f;

    for (size_t i = 1; i < output.size(); ++i)
        largestStep = juce::jmax (largestStep, std::abs (output[i] - output[i - 1]));

    // Over a 5 ms smoothing time the gain can move at most one 5 ms ramp's
    // worth per sample; a discontinuous parameter jump would be orders of
    // magnitude larger than this.
    const auto perSampleBudget = 1.0f / static_cast<float> (WetChain::duckAmountSmoothingSeconds * testSampleRate);

    INFO ("largest per-sample step " << largestStep << " against a budget of " << perSampleBudget * 2.0f);
    CHECK (largestStep < perSampleBudget * 2.0f);
    CHECK (largestStep < 0.02f);
}

TEST_CASE ("6.11 The wet chain never produces NaN or Inf under extreme settings", "[wetchain][robustness]")
{
    WetChain chain;
    chain.prepare (specFor (testSampleRate));
    chain.setLowCutHz (2000.0f);
    chain.setHighCutHz (1000.0f); // deliberately inverted: the bands do not overlap
    chain.setDuckAmountPercent (100.0f);
    chain.setDuckAttackMs (1.0f);
    chain.setDuckReleaseMs (50.0f);

    juce::Random random (23);

    const auto rendered = runChain (chain, static_cast<int> (testSampleRate),
                                     [&random] (int) { return (random.nextFloat() * 2.0f - 1.0f) * 4.0f; },
                                     [&random] (int) { return random.nextFloat() * 4.0f; });

    for (auto sample : rendered)
        REQUIRE (std::isfinite (sample));
}
