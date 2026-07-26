#include "MorphingConvolution.h"

#include <cmath>

namespace
{
    juce::dsp::Convolution::Stereo stereoFor (int numChannels)
    {
        return numChannels >= 2 ? juce::dsp::Convolution::Stereo::yes
                                 : juce::dsp::Convolution::Stereo::no;
    }
}

MorphingConvolution::MorphingConvolution()
{
    // Both engines share one background loading thread (juce::dsp::
    // ConvolutionMessageQueue), which is exactly what the shared-queue
    // constructor overload exists for. The queue must outlive both engines -
    // it is declared before them in the header, so destruction order is
    // correct.
    for (auto& engine : engines)
        engine = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::Latency { 0 }, messageQueue);
}

void MorphingConvolution::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    maximumBlockSize = static_cast<int> (spec.maximumBlockSize);
    preparedChannels = static_cast<int> (spec.numChannels);

    crossfadeLengthSamples = juce::jmax (1, static_cast<int> (std::round (crossfadeSeconds * sampleRate)));
    warmUpBlockLimit = juce::jmax (1, static_cast<int> (std::ceil (readinessFallbackSeconds * sampleRate
                                                                    / juce::jmax (1.0, static_cast<double> (maximumBlockSize)))));

    for (auto& engine : engines)
        engine->prepare (spec);

    idleOutput.setSize (juce::jmax (1, preparedChannels), juce::jmax (1, maximumBlockSize), false, true, true);

    reset();
}

void MorphingConvolution::reset()
{
    for (auto& engine : engines)
        engine->reset();

    idleOutput.clear();

    // Abandoning an in-flight swap on reset() is deliberate: reset() means
    // "start a new stream", so there is no old tail worth fading out of. The
    // live engine keeps whatever kernel it holds.
    state = State::steady;
    warmUpBlocksElapsed = 0;
    crossfadeSamplesElapsed = 0;
    expectedIdleIrSize = -1;
}

void MorphingConvolution::loadKernelSynchronously (juce::AudioBuffer<float>&& kernel, double kernelSampleRate,
                                                     int numChannels)
{
    engines[liveIndex]->loadImpulseResponse (std::move (kernel), kernelSampleRate, stereoFor (numChannels),
                                              juce::dsp::Convolution::Trim::no,
                                              juce::dsp::Convolution::Normalise::yes);

    state = State::steady;
    warmUpBlocksElapsed = 0;
    crossfadeSamplesElapsed = 0;
}

void MorphingConvolution::loadFileSynchronously (const juce::File& file, int numChannels)
{
    engines[liveIndex]->loadImpulseResponse (file, stereoFor (numChannels),
                                              juce::dsp::Convolution::Trim::yes, 0,
                                              juce::dsp::Convolution::Normalise::yes);

    state = State::steady;
    warmUpBlocksElapsed = 0;
    crossfadeSamplesElapsed = 0;
}

void MorphingConvolution::beginWarmUp (int expectedSize) noexcept
{
    expectedIdleIrSize = expectedSize;
    state = State::warmingUp;
    warmUpBlocksElapsed = 0;
    crossfadeSamplesElapsed = 0;
}

bool MorphingConvolution::postKernel (juce::AudioBuffer<float>&& kernel, double kernelSampleRate,
                                       int numChannels) noexcept
{
    if (! canAcceptKernel())
        return false;

    // The buffer's own length is the install sentinel. The caller applied
    // the alternating 0/1-sample zero pad off the audio thread, so this
    // length is guaranteed to differ from the previously posted kernel's
    // even when the underlying IR is unchanged in length (a Damping-only
    // change regenerates an IR of exactly the same length).
    const auto expectedSize = kernel.getNumSamples();

    idleIrSizeBeforeLoad = engines[idleIndex()]->getCurrentIRSize();

    engines[idleIndex()]->loadImpulseResponse (std::move (kernel), kernelSampleRate, stereoFor (numChannels),
                                                juce::dsp::Convolution::Trim::no,
                                                juce::dsp::Convolution::Normalise::yes);

    beginWarmUp (expectedSize);
    return true;
}

bool MorphingConvolution::postFile (const juce::File& file, int numChannels) noexcept
{
    if (! canAcceptKernel())
        return false;

    idleIrSizeBeforeLoad = engines[idleIndex()]->getCurrentIRSize();

    engines[idleIndex()]->loadImpulseResponse (file, stereoFor (numChannels),
                                                juce::dsp::Convolution::Trim::yes, 0,
                                                juce::dsp::Convolution::Normalise::yes);

    // -1: the post-load length of a file IR is not knowable up front (JUCE
    // resamples and optionally trims it), so readiness falls back to "the
    // reported size changed", still bounded by the 500 ms limit.
    beginWarmUp (-1);
    return true;
}

void MorphingConvolution::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = static_cast<int> (block.getNumSamples());
    const auto numChannels = static_cast<int> (block.getNumChannels());

    if (numSamples <= 0 || numChannels <= 0)
        return;

    //==========================================================================
    // Steady state: exactly one engine runs and its output is the output.
    // No crossfade multiply, no copy through a scratch buffer - this path is
    // bit-identical to a plain single juce::dsp::Convolution.
    if (state == State::steady)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        engines[liveIndex]->process (context);
        return;
    }

    const auto usableChannels = juce::jmin (numChannels, idleOutput.getNumChannels());
    const auto usableSamples = juce::jmin (numSamples, idleOutput.getNumSamples());

    // Defensive: a host handing a block larger than the one declared to
    // prepare() would overrun the scratch buffers. Fall back to the
    // single-engine path rather than reading out of bounds.
    if (usableSamples < numSamples || usableChannels < numChannels)
    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        engines[liveIndex]->process (context);
        return;
    }

    // Both engines must see the same input, so snapshot it before the live
    // engine overwrites `block` in place.
    for (int channel = 0; channel < numChannels; ++channel)
        idleOutput.copyFrom (channel, 0, block.getChannelPointer (static_cast<size_t> (channel)), numSamples);

    {
        juce::dsp::ProcessContextReplacing<float> context (block);
        engines[liveIndex]->process (context);
    }

    {
        auto idleBlock = juce::dsp::AudioBlock<float> (idleOutput).getSubBlock (0, static_cast<size_t> (numSamples))
                             .getSubsetChannelBlock (0, static_cast<size_t> (numChannels));
        juce::dsp::ProcessContextReplacing<float> idleContext (idleBlock);
        engines[idleIndex()]->process (idleContext);
    }

    //==========================================================================
    // Warm-up: the idle engine has been fed, its output is discarded, and we
    // watch getCurrentIRSize() for the length sentinel posted with the
    // kernel. Output stays 100% live engine, so the null invariant holds
    // right up to the moment theta starts moving.
    if (state == State::warmingUp)
    {
        ++warmUpBlocksElapsed;

        const auto reportedSize = engines[idleIndex()]->getCurrentIRSize();
        const auto sentinelMatched = expectedIdleIrSize > 0
                                       ? reportedSize == expectedIdleIrSize
                                       : (reportedSize > 0 && reportedSize != idleIrSizeBeforeLoad);

        if (sentinelMatched)
        {
            // theta begins moving on the *next* block, never this one.
            state = State::crossfading;
            crossfadeSamplesElapsed = 0;
        }
        else if (warmUpBlocksElapsed >= warmUpBlockLimit)
        {
            // Conservative fallback (brief 3.1 step 3): never hang a swap
            // just because a future JUCE version reports IR sizes
            // differently. Audible worst case is one hard-ish swap.
            jassertfalse;
            state = State::crossfading;
            crossfadeSamplesElapsed = 0;
        }

        return;
    }

    //==========================================================================
    // Crossfade: equal-power output blend, theta linear 0 -> pi/2.
    jassert (state == State::crossfading);

    const auto scale = juce::MathConstants<float>::halfPi / static_cast<float> (crossfadeLengthSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto position = juce::jmin (crossfadeSamplesElapsed + i, crossfadeLengthSamples);
        const auto theta = static_cast<float> (position) * scale;
        const auto oldGain = std::cos (theta);
        const auto newGain = std::sin (theta);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* out = block.getChannelPointer (static_cast<size_t> (channel));
            out[i] = oldGain * out[i] + newGain * idleOutput.getSample (channel, i);
        }
    }

    crossfadeSamplesElapsed += numSamples;

    if (crossfadeSamplesElapsed >= crossfadeLengthSamples)
    {
        // The idle engine is now the live one. The former live engine keeps
        // its (now stale) kernel and stops being processed until the next
        // swap posts a new one into it.
        liveIndex = idleIndex();
        state = State::steady;
        crossfadeSamplesElapsed = 0;
        warmUpBlocksElapsed = 0;
        expectedIdleIrSize = -1;
    }
}
