#include "dsp/ImpulseResponseGenerator.h"
#include "dsp/MorphingConvolution.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
// 6.3(b) - the click detector. A kernel swap under a sustained tone must not
// produce a discontinuity a listener would hear as a transient.
//
// WHAT THE ASSERTION IS PROTECTING. Feed a sustained 440 Hz sine; both engines
// are LTI, so once the old kernel's tail has fully built up the output is a
// *pure* 440 Hz sine. A band-limited sine of peak A cannot slew faster than
//
//     maxStep = 2 * sin(pi * f / fs) * A          per sample
//
// and the settled output sits exactly on that bound - measured at 1.000006x it,
// bit-identically on every run, because it is an algebraic identity rather than
// a statistic. That is the reference the swap is judged against: how many times
// faster than a tone at its own level did the output slew while the kernel was
// exchanged. Removing the equal-power ramp makes the cold idle engine's onset
// transient appear at full level instead of being windowed in, which is a
// ~9x level jump - and that is exactly what this test refuses.
//
// WHY THIS IS NOT A RACE. The previous version of this test compared a max step
// gathered over a fixed 400-block window against a "steady-state" baseline
// gathered over the preceding 150 blocks. Neither window was steady: at 150
// blocks (0.4 s) the 2 s tail is still building, and the 400-block window
// caught a scheduler-dependent amount of the 6 s tail's build-up. It therefore
// compared a still-building tail against a still-building tail, and the 3x
// tolerance it needed had 0.55% of headroom - it turned main red at aa868b5 on
// a run with no code change. Two things fix that, and neither is a nudged
// constant:
//
//   1. The reference is the *settled* tone immediately before the swap, whose
//      slew is known in closed form, instead of a co-measured noisy baseline.
//   2. The measurement window is anchored to the swap *in audio time*. Warm-up
//      leaves the output 100% the live engine, so the block on which
//      isWarmingUp() goes false is the last one rendered from the old kernel
//      alone, and the swap therefore begins at exactly that sample index -
//      however many blocks the background loader happened to need. Scheduler
//      jitter changes how long warm-up takes; it no longer moves the window
//      relative to the audio event.
//
// Only the warm-up loop sleeps, and only because juce::dsp::Convolution
// (JUCE 8.0.14) installs a posted kernel on its own background thread and
// offers no completion callback. Everything before and after it runs flat out.
//
// MEASURED HEADROOM, sweeping the swap phase across 33 distinct block anchors
// (warm-up 2..16 blocks, machine idle and loaded to 2x core count):
//
//     equal-power ramp intact : 1.71 .. 2.53   (bound 5.0, i.e. 2.0x headroom)
//     ramp replaced by a hard
//     switch (mechanism gone) : 9.21 .. 167.6  (worst case still 1.8x over)
TEST_CASE ("6.3 Swapping kernels under a sustained sine produces no click", "[morph][crossfade]")
{
    constexpr double toneHz = 440.0;
    constexpr float toneAmplitude = 0.5f;

    // Times the band-limited peak, this is the largest per-sample step a sine
    // at toneHz can take. Zoelzer, DAFX 2nd ed., sec. 1.2: differentiating
    // A*sin(wn) gives |y[n]-y[n-1]| = 2A*sin(w/2)*|cos(w(n-1/2))|.
    const auto slewPerUnitPeak = 2.0f * std::sin (juce::MathConstants<float>::pi
                                                    * static_cast<float> (toneHz / testSampleRate));

    // How many times a settled tone's own slew the output may reach while the
    // kernel is exchanged. See the measured spread above.
    constexpr float clickBudget = 5.0f;

    MorphingConvolution morph;
    morph.loadKernelSynchronously (makeKernel (2.0f, 8000.0f, 0), testSampleRate, 2);
    morph.prepare (specFor (testSampleRate));

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> rendered;
    rendered.reserve (400000);

    juce::int64 sampleIndex = 0;

    const auto pushBlock = [&] (bool sleep)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, toneHz, toneAmplitude, sampleIndex);
        sampleIndex += testBlockSize;

        auto audioBlock = juce::dsp::AudioBlock<float> (buffer);
        morph.process (audioBlock);

        for (int i = 0; i < testBlockSize; ++i)
            rendered.push_back (buffer.getSample (0, i));

        if (sleep)
            juce::Thread::sleep (1);
    };

    const auto maxStepOver = [&] (size_t from, size_t to)
    {
        auto maxStep = 0.0f;

        for (auto i = juce::jmax (size_t (1), from); i < juce::jmin (to, rendered.size()); ++i)
            maxStep = juce::jmax (maxStep, std::abs (rendered[i] - rendered[i - 1]));

        return maxStep;
    };

    const auto peakOver = [&] (size_t from, size_t to)
    {
        auto peak = 0.0f;

        for (auto i = from; i < juce::jmin (to, rendered.size()); ++i)
            peak = juce::jmax (peak, std::abs (rendered[i]));

        return peak;
    };

    //==========================================================================
    // The initial kernel is installed by the same background thread, so wait
    // for it on the handshake rather than on a timer. After this point nothing
    // in the test is waiting on another thread until the swap is posted.
    auto installBlocks = 0;

    while (morph.getLiveIrSize() <= 0 && installBlocks < 4000)
    {
        pushBlock (true);
        ++installBlocks;
    }

    REQUIRE (morph.getLiveIrSize() > 0);

    // Settle the 2 s tail completely - 3.2 s, flat out.
    for (int block = 0; block < 1200; ++block)
        pushBlock (false);

    //==========================================================================
    // Decay 2 s -> 6 s, the brief's swap. Warm-up renders 100% the live engine,
    // so these blocks are still settled tone and still part of the reference.
    REQUIRE (morph.postKernel (makeKernel (6.0f, 8000.0f, 1), testSampleRate, 2));

    auto warmUpBlocks = 0;

    while (morph.isWarmingUp() && warmUpBlocks < 4000)
    {
        pushBlock (true);
        ++warmUpBlocks;
    }

    REQUIRE_FALSE (morph.isWarmingUp());

    // The swap begins here, exactly: the block above was the last one rendered
    // from the live engine alone.
    const auto swapAnchor = rendered.size();

    const auto fadeSamples = static_cast<size_t> (MorphingConvolution::crossfadeSeconds * testSampleRate);
    const auto tailSamples = static_cast<size_t> (0.5 * testSampleRate);

    for (auto pushed = size_t (0); pushed < fadeSamples + tailSamples; pushed += testBlockSize)
        pushBlock (false);

    //==========================================================================
    // The reference: the half second of settled tone immediately before the swap.
    const auto referenceFrom = swapAnchor - static_cast<size_t> (0.5 * testSampleRate);
    const auto referenceHalf = referenceFrom + static_cast<size_t> (0.25 * testSampleRate);

    const auto settledPeakEarly = peakOver (referenceFrom, referenceHalf);
    const auto settledPeak = peakOver (referenceHalf, swapAnchor);
    const auto settledMaxStep = maxStepOver (referenceFrom, swapAnchor);

    REQUIRE (settledPeak > 0.0f);

    // The tail really has stopped building - otherwise the reference below is
    // measuring a transient, which is the trap the previous version fell into.
    INFO ("settled peak: first half " << settledPeakEarly << ", second half " << settledPeak);
    CHECK (std::abs (settledPeakEarly - settledPeak) <= 0.01f * settledPeak);

    const auto baselineStep = slewPerUnitPeak * settledPeak;

    // ...and it really is a band-limited tone sitting on its own slew bound,
    // which is what makes `baselineStep` an exact reference rather than a
    // statistic. Measured 1.000006 on every run, macOS and Windows.
    INFO ("settled max step " << settledMaxStep << " vs band-limited bound " << baselineStep);
    CHECK (settledMaxStep > 0.9f * baselineStep);
    CHECK (settledMaxStep < 1.05f * baselineStep);

    //==========================================================================
    const auto swapMaxStep = maxStepOver (swapAnchor, swapAnchor + fadeSamples + tailSamples);

    INFO ("swap began at sample " << swapAnchor << " after " << warmUpBlocks << " warm-up blocks; "
           << "max step across the swap " << swapMaxStep << " = "
           << (swapMaxStep / baselineStep) << "x the settled tone's own slew (budget "
           << clickBudget << "x)");
    CHECK (swapMaxStep < clickBudget * baselineStep);

    const auto allFinite = std::all_of (rendered.begin(), rendered.end(),
                                         [] (float sample) { return std::isfinite (sample); });
    CHECK (allFinite);
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
