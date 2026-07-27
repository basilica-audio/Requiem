#include "FdnTail.h"

#include <cmath>

namespace
{
    bool isPrime (int value) noexcept
    {
        if (value < 2)
            return false;

        if (value % 2 == 0)
            return value == 2;

        for (int divisor = 3; divisor * divisor <= value; divisor += 2)
            if (value % divisor == 0)
                return false;

        return true;
    }

    // Nearest prime at or above `value` that is not already taken. Prime
    // lengths are pairwise coprime, which is the classic condition for the
    // network's modes not to collapse onto common periods (Schroeder 1962;
    // Jot 1991) - the difference between a diffuse tail and a metallic one.
    int nextUnusedPrime (int value, const std::array<int, FdnTail::numLines>& taken, int count) noexcept
    {
        auto candidate = juce::jmax (2, value);

        for (;;)
        {
            if (isPrime (candidate))
            {
                auto alreadyTaken = false;

                for (int i = 0; i < count; ++i)
                    alreadyTaken = alreadyTaken || taken[static_cast<size_t> (i)] == candidate;

                if (! alreadyTaken)
                    return candidate;
            }

            ++candidate;
        }
    }

    // Cubic Hermite (Catmull-Rom) fractional-delay interpolation - the
    // interpolator Lush mode's modulated reads need to avoid the audible
    // high-frequency loss linear interpolation would impose on a tail that
    // recirculates through it thousands of times.
    float hermite (float fraction, float yMinus1, float y0, float y1, float y2) noexcept
    {
        const auto c0 = y0;
        const auto c1 = 0.5f * (y1 - yMinus1);
        const auto c2 = yMinus1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const auto c3 = 0.5f * (y2 - yMinus1) + 1.5f * (y0 - y1);

        return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
    }
}

FdnTail::FdnTail()
{
    // Deterministic, seeded tap/modulation pattern: the tail must sound the
    // same in every instance and every render, and the tests must be able to
    // measure it reproducibly.
    juce::Random random (0x5EEDF0Du);

    for (int line = 0; line < numLines; ++line)
    {
        injectionTaps[static_cast<size_t> (line)] = random.nextBool() ? 1.0f : -1.0f;
        outputTapsLeft[static_cast<size_t> (line)] = random.nextBool() ? 1.0f : -1.0f;
    }

    // c_R is built to be orthogonal to c_L: flipping exactly half of c_L's
    // signs makes the two tap vectors' inner product zero, so the two output
    // channels are decorrelated rather than merely different.
    outputTapsRight = outputTapsLeft;

    for (int line = 0; line < numLines / 2; ++line)
        outputTapsRight[static_cast<size_t> (line * 2)] *= -1.0f;

    const auto tapScale = 1.0f / std::sqrt (static_cast<float> (numLines));

    for (int line = 0; line < numLines; ++line)
    {
        injectionTaps[static_cast<size_t> (line)] *= tapScale;
        outputTapsLeft[static_cast<size_t> (line)] *= tapScale;
        outputTapsRight[static_cast<size_t> (line)] *= tapScale;
    }

    for (int pair = 0; pair < numLines / 2; ++pair)
    {
        rotationRatesHz[static_cast<size_t> (pair)] =
            juce::jmap (random.nextFloat(), 0.0f, 1.0f, minMatrixRateHz, maxMatrixRateHz);
        rotationPhases[static_cast<size_t> (pair)] = random.nextFloat() * juce::MathConstants<float>::twoPi;
        rotationCos[static_cast<size_t> (pair)] = 1.0f;
        rotationSin[static_cast<size_t> (pair)] = 0.0f;
    }

    for (int line = 0; line < numLines; ++line)
    {
        lushRatesHz[static_cast<size_t> (line)] =
            juce::jmap (random.nextFloat(), 0.0f, 1.0f, minLushRateHz, maxLushRateHz);
        lushPhases[static_cast<size_t> (line)] = random.nextFloat() * juce::MathConstants<float>::twoPi;
        lushOffsets[static_cast<size_t> (line)] = 0.0f;
    }
}

void FdnTail::primeDelayLengths (double newSampleRate)
{
    std::array<int, numLines> lengths {};

    for (int line = 0; line < numLines; ++line)
    {
        // Log-spaced between the two bounds: equal *ratios* between adjacent
        // lines, which spreads the modal series evenly rather than crowding
        // the long end.
        const auto t = numLines > 1 ? static_cast<float> (line) / static_cast<float> (numLines - 1) : 0.0f;
        const auto milliseconds = minDelayMs * std::pow (maxDelayMs / minDelayMs, t);
        const auto ideal = static_cast<int> (std::round (milliseconds * 0.001 * newSampleRate));

        lengths[static_cast<size_t> (line)] = nextUnusedPrime (ideal, lengths, line);
    }

    delayLengths = lengths;
    shortestDelaySamples = delayLengths[0];

    for (auto length : delayLengths)
        shortestDelaySamples = juce::jmin (shortestDelaySamples, length);
}

void FdnTail::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannels = static_cast<int> (spec.numChannels);

    primeDelayLengths (sampleRate);

    for (int line = 0; line < numLines; ++line)
    {
        // A few samples of headroom past the nominal length so Lush mode's
        // modulated reads (and the Hermite interpolator's four-point window)
        // always stay inside the buffer.
        const auto capacity = delayLengths[static_cast<size_t> (line)] + 16;
        lineBuffers[static_cast<size_t> (line)].assign (static_cast<size_t> (capacity), 0.0f);
        writePositions[static_cast<size_t> (line)] = 0;
    }

    modulationPhaseIncrementBase = juce::MathConstants<double>::twoPi / sampleRate;
    samplesUntilModulationUpdate = 0;

    // Safety net: a default-constructed LineAttenuation is the *identity*,
    // and an FDN with identity attenuation is a lossless prototype - it would
    // sustain forever. Until the design thread has fitted a real RT60(f)
    // curve, every line therefore gets a plain broadband gain corresponding
    // to a 2 s decay, so a caller that forgets to post coefficients gets a
    // dull reverb rather than a runaway one.
    if (! hasDesignedAttenuation)
    {
        for (int line = 0; line < numLines; ++line)
        {
            const auto perSampleDb = AttenuationDesign::perSampleAttenuationDb (2.0f, sampleRate);
            const auto lineDb = perSampleDb * static_cast<float> (delayLengths[static_cast<size_t> (line)]);

            attenuationSets[static_cast<size_t> (line)] = AttenuationDesign::LineAttenuation {};
            attenuationSets[static_cast<size_t> (line)].broadbandGain = juce::Decibels::decibelsToGain (lineDb);
        }
    }

    freezeAmount.reset (sampleRate, freezeRampSeconds);
    freezeAmount.setCurrentAndTargetValue (frozen ? 1.0f : 0.0f);

    reset();
}

void FdnTail::reset()
{
    for (auto& buffer : lineBuffers)
        std::fill (buffer.begin(), buffer.end(), 0.0f);

    for (auto& position : writePositions)
        position = 0;

    for (auto& lineStates : filterStates)
        for (auto& sectionState : lineStates)
            sectionState = { 0.0f, 0.0f };

    for (auto& offset : lushOffsets)
        offset = 0.0f;

    for (int pair = 0; pair < numLines / 2; ++pair)
    {
        rotationCos[static_cast<size_t> (pair)] = 1.0f;
        rotationSin[static_cast<size_t> (pair)] = 0.0f;
    }

    samplesUntilModulationUpdate = 0;
    freezeAmount.setCurrentAndTargetValue (frozen ? 1.0f : 0.0f);
}

int FdnTail::getTotalDelaySamples() const noexcept
{
    auto total = 0;

    for (auto length : delayLengths)
        total += length;

    return total;
}

void FdnTail::postAttenuation (const std::array<AttenuationDesign::LineAttenuation, numLines>& attenuation) noexcept
{
    const juce::SpinLock::ScopedLockType lock (pendingAttenuationLock);
    pendingAttenuation = attenuation;
    hasPendingAttenuation = true;
    hasDesignedAttenuation = true;
}

void FdnTail::setAttenuationImmediately (const std::array<AttenuationDesign::LineAttenuation, numLines>& attenuation)
{
    attenuationSets = attenuation;
    hasDesignedAttenuation = true;

    const juce::SpinLock::ScopedLockType lock (pendingAttenuationLock);
    hasPendingAttenuation = false;
}

void FdnTail::fetchPendingAttenuationIfAny() noexcept
{
    // Try-lock only: if the design thread happens to be mid-write, this block
    // simply keeps the previous coefficients and picks the new ones up next
    // block. Design updates arrive at ~100 Hz; blocks run far faster.
    const juce::SpinLock::ScopedTryLockType lock (pendingAttenuationLock);

    if (! lock.isLocked() || ! hasPendingAttenuation)
        return;

    attenuationSets = pendingAttenuation;
    hasPendingAttenuation = false;
}

void FdnTail::setModulationMode (ModulationMode newMode) noexcept
{
    modulationMode = newMode;
}

void FdnTail::setModulationDepth (float newDepth01) noexcept
{
    modulationDepth01 = juce::jlimit (0.0f, 1.0f, newDepth01);
}

void FdnTail::setModulationRateScale (float newScale01Based) noexcept
{
    modulationRateScale = juce::jlimit (0.05f, 8.0f, newScale01Based);
}

void FdnTail::setFrozen (bool shouldFreeze) noexcept
{
    if (frozen == shouldFreeze)
        return;

    frozen = shouldFreeze;
    freezeAmount.setTargetValue (frozen ? 1.0f : 0.0f);
}

void FdnTail::updateModulation() noexcept
{
    // Control-rate update: the rotation angles (and Lush's delay offsets) are
    // recomputed once per sub-block, never per sample, so the eight sin/cos
    // pairs cost nothing measurable against sixteen delay lines and a hundred
    // and sixty biquads.
    const auto depth = modulationMode == ModulationMode::off ? 0.0f : modulationDepth01;

    if (modulationMode == ModulationMode::matrix)
    {
        const auto maxAngle = juce::degreesToRadians (maxRotationDegrees) * depth;

        for (int pair = 0; pair < numLines / 2; ++pair)
        {
            const auto phase = rotationPhases[static_cast<size_t> (pair)];
            const auto angle = maxAngle * std::sin (phase);

            rotationCos[static_cast<size_t> (pair)] = std::cos (angle);
            rotationSin[static_cast<size_t> (pair)] = std::sin (angle);
        }
    }
    else
    {
        for (int pair = 0; pair < numLines / 2; ++pair)
        {
            rotationCos[static_cast<size_t> (pair)] = 1.0f;
            rotationSin[static_cast<size_t> (pair)] = 0.0f;
        }
    }

    if (modulationMode == ModulationMode::lush)
    {
        // Depth scales with the sample rate so the modulation is the same
        // musical interval at 48 k and at 192 k, not the same sample count.
        const auto depthSamples = maxLushDepthSamplesAt48k * depth * static_cast<float> (sampleRate / 48000.0);

        for (int line = 0; line < numLines; ++line)
            lushOffsets[static_cast<size_t> (line)] = depthSamples * std::sin (lushPhases[static_cast<size_t> (line)]);
    }
    else
    {
        for (auto& offset : lushOffsets)
            offset = 0.0f;
    }

    // Advance the LFO phases by one sub-block.
    const auto advance = static_cast<float> (modulationPhaseIncrementBase * modulationSubBlockSamples);

    for (int pair = 0; pair < numLines / 2; ++pair)
    {
        rotationPhases[static_cast<size_t> (pair)] +=
            advance * rotationRatesHz[static_cast<size_t> (pair)] * modulationRateScale;

        if (rotationPhases[static_cast<size_t> (pair)] > juce::MathConstants<float>::twoPi)
            rotationPhases[static_cast<size_t> (pair)] -= juce::MathConstants<float>::twoPi;
    }

    for (int line = 0; line < numLines; ++line)
    {
        lushPhases[static_cast<size_t> (line)] +=
            advance * lushRatesHz[static_cast<size_t> (line)] * modulationRateScale;

        if (lushPhases[static_cast<size_t> (line)] > juce::MathConstants<float>::twoPi)
            lushPhases[static_cast<size_t> (line)] -= juce::MathConstants<float>::twoPi;
    }
}

float FdnTail::readLine (int line, float delaySamples) const noexcept
{
    const auto& buffer = lineBuffers[static_cast<size_t> (line)];
    const auto capacity = static_cast<int> (buffer.size());

    if (capacity <= 4)
        return 0.0f;

    const auto clamped = juce::jlimit (2.0f, static_cast<float> (capacity - 3), delaySamples);
    const auto integerPart = static_cast<int> (clamped);
    const auto fraction = clamped - static_cast<float> (integerPart);

    const auto base = writePositions[static_cast<size_t> (line)] - integerPart + capacity;

    const auto at = [&] (int offset)
    {
        return buffer[static_cast<size_t> ((base + offset) % capacity)];
    };

    // Reading "one sample earlier" means a *shorter* delay, hence +1 here.
    return hermite (fraction, at (1), at (0), at (-1 + capacity), at (-2 + capacity));
}

void FdnTail::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = static_cast<int> (block.getNumSamples());
    const auto blockChannels = static_cast<int> (block.getNumChannels());

    if (numSamples <= 0 || blockChannels <= 0)
        return;

    fetchPendingAttenuationIfAny();

    const auto interpolatedReads = modulationMode == ModulationMode::lush;

    auto* left = block.getChannelPointer (0);
    auto* right = blockChannels >= 2 ? block.getChannelPointer (1) : nullptr;

    std::array<float, numLines> lineOutputs {};
    std::array<float, numLines> filtered {};

    for (int i = 0; i < numSamples; ++i)
    {
        if (samplesUntilModulationUpdate <= 0)
        {
            updateModulation();
            samplesUntilModulationUpdate = modulationSubBlockSamples;
        }

        --samplesUntilModulationUpdate;

        const auto freeze = freezeAmount.getNextValue();
        const auto inputGain = 1.0f - freeze;

        // Mono-sum injection: the tail's stereo image comes from the two
        // decorrelated output tap vectors, not from feeding two independent
        // networks, which would double the cost for no audible gain.
        const auto input = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];

        //======================================================================
        // Read the delay lines.
        for (int line = 0; line < numLines; ++line)
        {
            const auto nominal = static_cast<float> (delayLengths[static_cast<size_t> (line)]);

            lineOutputs[static_cast<size_t> (line)] =
                interpolatedReads ? readLine (line, nominal + lushOffsets[static_cast<size_t> (line)])
                                   : lineBuffers[static_cast<size_t> (line)]
                                         [static_cast<size_t> ((writePositions[static_cast<size_t> (line)]
                                                                 - delayLengths[static_cast<size_t> (line)]
                                                                 + static_cast<int> (lineBuffers[static_cast<size_t> (line)].size()))
                                                                % static_cast<int> (lineBuffers[static_cast<size_t> (line)].size()))];
        }

        //======================================================================
        // Output taps: read only the delay outputs, never the input - hence
        // no direct feedthrough, hence the intrinsic onset delay documented
        // in the header.
        auto outLeft = 0.0f;
        auto outRight = 0.0f;

        for (int line = 0; line < numLines; ++line)
        {
            outLeft += outputTapsLeft[static_cast<size_t> (line)] * lineOutputs[static_cast<size_t> (line)];
            outRight += outputTapsRight[static_cast<size_t> (line)] * lineOutputs[static_cast<size_t> (line)];
        }

        //======================================================================
        // Per-line attenuation. While frozen the cascade is crossfaded out to
        // unity, leaving the lossless prototype - which is what makes Freeze
        // hold exactly, indefinitely, rather than merely decaying slowly.
        for (int line = 0; line < numLines; ++line)
        {
            auto sample = lineOutputs[static_cast<size_t> (line)];
            const auto& attenuation = attenuationSets[static_cast<size_t> (line)];
            auto& states = filterStates[static_cast<size_t> (line)];

            for (int section = 0; section < AttenuationDesign::numBands; ++section)
            {
                const auto& biquad = attenuation.sections[static_cast<size_t> (section)];
                auto& state = states[static_cast<size_t> (section)];

                // Transposed direct form II: the numerically better-behaved
                // topology for cascaded biquads at low command gains.
                const auto output = biquad.b0 * sample + state[0];
                state[0] = biquad.b1 * sample - biquad.a1 * output + state[1];
                state[1] = biquad.b2 * sample - biquad.a2 * output;
                sample = output;
            }

            sample *= attenuation.broadbandGain;

            filtered[static_cast<size_t> (line)] =
                sample + freeze * (lineOutputs[static_cast<size_t> (line)] - sample);
        }

        //======================================================================
        // Householder reflection A = I - (2/N) 1 1^T: one sum and N fused
        // multiply-adds, and orthogonal (indeed involutory) by construction.
        auto sum = 0.0f;

        for (int line = 0; line < numLines; ++line)
            sum += filtered[static_cast<size_t> (line)];

        const auto householderScale = 2.0f / static_cast<float> (numLines) * sum;

        for (int line = 0; line < numLines; ++line)
            filtered[static_cast<size_t> (line)] -= householderScale;

        //======================================================================
        // Time-varying Givens rotations on disjoint index pairs. Orthogonal at
        // every instant, so the product with the Householder reflection is
        // still orthogonal: stability is guaranteed and no delay length has
        // moved, so there is no pitch modulation to speak of.
        if (modulationMode == ModulationMode::matrix)
        {
            for (int pair = 0; pair < numLines / 2; ++pair)
            {
                const auto a = static_cast<size_t> (pair * 2);
                const auto b = a + 1;
                const auto c = rotationCos[static_cast<size_t> (pair)];
                const auto s = rotationSin[static_cast<size_t> (pair)];

                const auto x = filtered[a];
                const auto y = filtered[b];

                filtered[a] = c * x - s * y;
                filtered[b] = s * x + c * y;
            }
        }

        //======================================================================
        // Write back, with the input injected through the +/-1 tap vector.
        for (int line = 0; line < numLines; ++line)
        {
            auto& buffer = lineBuffers[static_cast<size_t> (line)];
            const auto capacity = static_cast<int> (buffer.size());

            buffer[static_cast<size_t> (writePositions[static_cast<size_t> (line)])] =
                filtered[static_cast<size_t> (line)]
                + inputGain * injectionTaps[static_cast<size_t> (line)] * input;

            writePositions[static_cast<size_t> (line)] =
                (writePositions[static_cast<size_t> (line)] + 1) % capacity;
        }

        left[i] = outLeft;

        if (right != nullptr)
            right[i] = outRight;
    }

    // Any further channels beyond stereo carry the left tap - the plugin's
    // bus layout only ever presents mono or stereo, so this is belt and braces.
    for (int channel = 2; channel < blockChannels; ++channel)
        juce::FloatVectorOperations::copy (block.getChannelPointer (static_cast<size_t> (channel)),
                                            left, numSamples);
}
