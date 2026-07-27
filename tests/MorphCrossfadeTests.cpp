#include "dsp/ImpulseResponseGenerator.h"
#include "dsp/MorphingConvolution.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <vector>

// v0.3.0 brief test 6.3: the A/B convolution morph and its readiness
// handshake. These tests deliberately run in something close to real time -
// juce::dsp::Convolution installs a posted kernel on its own background
// thread, and an offline render pushes blocks through far faster than that
// thread can be scheduled, so a test that spun the block loop flat out would
// be measuring the scheduler rather than the crossfade.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 128;

    juce::dsp::ProcessSpec specFor (double sampleRate, int blockSize = testBlockSize)
    {
        return { sampleRate, static_cast<juce::uint32> (blockSize), 2 };
    }

    juce::AudioBuffer<float> makeKernel (float decaySeconds, float dampingHz, int trailingPad)
    {
        auto kernel = ReverbIR::generateProceduralImpulseResponse (testSampleRate, decaySeconds, dampingHz, 2,
                                                                    ReverbIR::SpaceType::hall, 0.8f, false, 1,
                                                                    0.5f, 1.3f);

        if (trailingPad > 0)
            kernel.setSize (kernel.getNumChannels(), kernel.getNumSamples() + trailingPad, true, true, true);

        return kernel;
    }

    // Pushes blocks through until `predicate` holds, sleeping between blocks so
    // the convolution's background loader actually gets scheduled. Returns the
    // number of blocks processed, or -1 on timeout.
    int processUntil (MorphingConvolution& morph, juce::AudioBuffer<float>& buffer,
                       const std::function<bool()>& predicate, int maxBlocks = 4000)
    {
        for (int block = 0; block < maxBlocks; ++block)
        {
            if (predicate())
                return block;

            buffer.clear();
            buffer.setSample (0, 0, 1.0f);
            buffer.setSample (1, 0, 1.0f);

            auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
            morph.process (audioBlock);

            juce::Thread::sleep (1);
        }

        return -1;
    }
}

//==============================================================================
// 6.3(a) - the steady-state null. With one engine live and no swap in flight,
// the morph front-end must be bit-identical to a plain single convolution.
TEST_CASE ("6.3 In steady state the morph is bit-identical to a single convolution", "[morph][null]")
{
    const auto spec = specFor (testSampleRate);

    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (spec);

    juce::dsp::Convolution reference;
    reference.loadImpulseResponse (makeKernel (2.0f, 8000.0f, 0), testSampleRate,
                                    juce::dsp::Convolution::Stereo::yes,
                                    juce::dsp::Convolution::Trim::no,
                                    juce::dsp::Convolution::Normalise::yes);
    reference.prepare (spec);

    REQUIRE_FALSE (morph.isCrossfading());
    REQUIRE_FALSE (morph.isWarmingUp());

    juce::AudioBuffer<float> morphBuffer (2, testBlockSize);
    juce::AudioBuffer<float> referenceBuffer (2, testBlockSize);
    juce::Random random (41);

    auto maxDifference = 0.0f;

    for (int block = 0; block < 200; ++block)
    {
        for (int i = 0; i < testBlockSize; ++i)
        {
            const auto sample = random.nextFloat() * 2.0f - 1.0f;
            morphBuffer.setSample (0, i, sample);
            morphBuffer.setSample (1, i, sample);
            referenceBuffer.setSample (0, i, sample);
            referenceBuffer.setSample (1, i, sample);
        }

        auto morphBlock = juce::dsp::AudioBlock<float> (morphBuffer);
        morph.process (morphBlock);

        juce::dsp::AudioBlock<float> referenceBlock (referenceBuffer);
        juce::dsp::ProcessContextReplacing<float> referenceContext (referenceBlock);
        reference.process (referenceContext);

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < testBlockSize; ++i)
                maxDifference = juce::jmax (maxDifference,
                                             std::abs (morphBuffer.getSample (channel, i)
                                                        - referenceBuffer.getSample (channel, i)));
    }

    INFO ("max difference " << maxDifference);
    CHECK (maxDifference < 1.0e-6f);
}

//==============================================================================
// 6.3(c) - the crossfade only starts once the idle engine demonstrably holds
// the new kernel, and then runs for 100 ms.
TEST_CASE ("6.3 The crossfade starts only after the handshake and lasts 100 ms", "[morph][crossfade]")
{
    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    // Warm the engine up so it is genuinely in steady state first.
    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);
    }

    REQUIRE (morph.canAcceptKernel());

    auto newKernel = makeKernel (6.0f, 8000.0f, 1);
    const auto newKernelLength = newKernel.getNumSamples();

    REQUIRE (morph.postKernel (std::move (newKernel), testSampleRate, 2));
    REQUIRE (morph.isWarmingUp());

    // Warm-up: output stays 100% the live engine, so a further kernel cannot
    // be accepted yet.
    CHECK_FALSE (morph.canAcceptKernel());

    const auto blocksToFadeStart = processUntil (morph, buffer, [&morph] { return morph.isCrossfading(); });
    REQUIRE (blocksToFadeStart >= 0);

    // The fade began only once the idle engine reported the new kernel's exact
    // (padded) length - the handshake, not a timer.
    INFO ("fade started after " << blocksToFadeStart << " warm-up blocks; kernel length " << newKernelLength);
    CHECK (blocksToFadeStart > 0);

    // Measure the fade's length from its first block.
    auto fadeBlocks = 0;

    while (morph.isCrossfading() && fadeBlocks < 4000)
    {
        buffer.clear();
        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);
        ++fadeBlocks;
    }

    CHECK_FALSE (morph.isCrossfading());
    CHECK (morph.canAcceptKernel());

    const auto expectedBlocks = static_cast<int> (std::ceil (MorphingConvolution::crossfadeSeconds
                                                              * testSampleRate / testBlockSize));

    INFO ("fade ran for " << fadeBlocks << " blocks, expected " << expectedBlocks);
    CHECK (std::abs (fadeBlocks - expectedBlocks) <= 1);
}

//==============================================================================
// 6.3(d) - the handshake stays deterministic when the new kernel has the same
// underlying length as the old one, which is exactly what a Damping-only
// change produces.
TEST_CASE ("6.3 A Damping-only change is still detected, via the length sentinel", "[morph][crossfade]")
{
    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);
    }

    // Same Decay, different Damping: the generator produces an impulse
    // response of *identical length*. Without the alternating trailing pad the
    // idle engine's reported size would be unchanged after the load and the
    // handshake could never tell "installed" from "still holding the old one".
    const auto oldLength = makeKernel (2.0f, 8000.0f, 0).getNumSamples();
    auto newKernel = makeKernel (2.0f, 3000.0f, 1);

    REQUIRE (newKernel.getNumSamples() == oldLength + 1);
    const auto liveSizeBefore = morph.getLiveIrSize();

    REQUIRE (morph.postKernel (std::move (newKernel), testSampleRate, 2));

    const auto blocksToFadeStart = processUntil (morph, buffer, [&morph] { return morph.isCrossfading(); });
    REQUIRE (blocksToFadeStart >= 0);

    // Drive the fade to completion, then confirm the live engine really is
    // holding the new kernel - i.e. the fade went somewhere, not nowhere.
    processUntil (morph, buffer, [&morph] { return ! morph.isCrossfading(); });

    INFO ("live IR size before " << liveSizeBefore << ", after " << morph.getLiveIrSize());
    CHECK (morph.getLiveIrSize() == oldLength + 1);
}

//==============================================================================
// 6.3(b) - the click detector. A kernel swap under a sustained sine must not
// produce a sample-to-sample step materially larger than the signal's own.
TEST_CASE ("6.3 Swapping kernels under a sustained sine produces no click", "[morph][crossfade]")
{
    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> rendered;
    rendered.reserve (200000);

    juce::int64 sampleIndex = 0;

    const auto pushBlocks = [&] (int numBlocks)
    {
        for (int block = 0; block < numBlocks; ++block)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f, sampleIndex);
            sampleIndex += testBlockSize;

            auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
            morph.process (audioBlock);

            for (int i = 0; i < testBlockSize; ++i)
                rendered.push_back (buffer.getSample (0, i));

            juce::Thread::sleep (1);
        }
    };

    // Steady state first, to establish the baseline step size.
    pushBlocks (150);
    const auto steadyStateSamples = rendered.size();

    auto steadyStateMaxStep = 0.0f;

    for (size_t i = steadyStateSamples / 2; i < steadyStateSamples; ++i)
        steadyStateMaxStep = juce::jmax (steadyStateMaxStep, std::abs (rendered[i] - rendered[i - 1]));

    REQUIRE (steadyStateMaxStep > 0.0f);

    // Decay 2 s -> 6 s, the brief's swap.
    REQUIRE (morph.postKernel (makeKernel (6.0f, 8000.0f, 1), testSampleRate, 2));
    pushBlocks (400);

    auto swapMaxStep = 0.0f;

    for (auto i = steadyStateSamples; i < rendered.size(); ++i)
        swapMaxStep = juce::jmax (swapMaxStep, std::abs (rendered[i] - rendered[i - 1]));

    INFO ("steady-state max step " << steadyStateMaxStep << ", across the swap " << swapMaxStep);
    CHECK (swapMaxStep < 3.0f * steadyStateMaxStep);

    for (auto sample : rendered)
        REQUIRE (std::isfinite (sample));
}

//==============================================================================
TEST_CASE ("6.3 The morph refuses a new kernel while a swap is already in flight", "[morph][crossfade]")
{
    // The caller is expected to leave a pending kernel in its own hand-off
    // slot rather than have this class buffer a second one - buffering would
    // mean freeing an AudioBuffer on the audio thread.
    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);
    }

    REQUIRE (morph.postKernel (makeKernel (4.0f, 8000.0f, 1), testSampleRate, 2));

    auto second = makeKernel (5.0f, 8000.0f, 0);
    CHECK_FALSE (morph.postKernel (std::move (second), testSampleRate, 2));

    // ...and the rejected buffer was left untouched, not consumed.
    CHECK (second.getNumSamples() > 0);
}

TEST_CASE ("6.3 The morph reports zero latency in every state", "[morph][latency]")
{
    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    CHECK (morph.getLatency() == 0);

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    for (int block = 0; block < 20; ++block)
    {
        buffer.clear();
        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);
    }

    REQUIRE (morph.postKernel (makeKernel (6.0f, 8000.0f, 1), testSampleRate, 2));
    CHECK (morph.getLatency() == 0);

    processUntil (morph, buffer, [&morph] { return morph.isCrossfading(); });
    CHECK (morph.getLatency() == 0);

    processUntil (morph, buffer, [&morph] { return morph.canAcceptKernel(); });
    CHECK (morph.getLatency() == 0);
}
