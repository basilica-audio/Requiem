#include "WetChain.h"

#include <cmath>

void WetChain::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    lowCutFilter.prepare (spec);
    lowCutFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    lowCutFilter.setResonance (juce::MathConstants<float>::sqrt2 * 0.5f);

    highCutFilter.prepare (spec);
    highCutFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    highCutFilter.setResonance (juce::MathConstants<float>::sqrt2 * 0.5f);

    // Nyquist guard: a TPT cutoff at or above Nyquist produces a degenerate
    // prewarped coefficient. The High Cut range top (20 kHz) is above
    // Nyquist at 32 kHz and below, but that setting is the *bypass* end so
    // the filter is never actually run there - this clamp only matters for
    // the smoothed values in between.
    const auto nyquistLimit = static_cast<float> (sampleRate * 0.5) * 0.95f;

    lowCutFilter.setCutoffFrequency (juce::jmin (lastLowCutHz, nyquistLimit));
    highCutFilter.setCutoffFrequency (juce::jmin (lastHighCutHz, nyquistLimit));

    lowCutSmoothed.reset (sampleRate, cutoffSmoothingSeconds);
    lowCutSmoothed.setCurrentAndTargetValue (lastLowCutHz);
    highCutSmoothed.reset (sampleRate, cutoffSmoothingSeconds);
    highCutSmoothed.setCurrentAndTargetValue (lastHighCutHz);

    duckAmountSmoothed.reset (sampleRate, duckAmountSmoothingSeconds);
    duckAmountSmoothed.setCurrentAndTargetValue (duckAmount01);

    updateEnvelopeCoefficients();

    reset();
}

void WetChain::reset()
{
    lowCutFilter.reset();
    highCutFilter.reset();
    envelope = 0.0f;
    duckAmountSmoothed.setCurrentAndTargetValue (duckAmount01);
}

void WetChain::updateEnvelopeCoefficients()
{
    // a = exp(-1 / (tau * fs)) - the standard one-pole time constant, so the
    // envelope reaches 1 - 1/e (63%) of a step after exactly tau seconds.
    const auto coefficientFor = [this] (float milliseconds)
    {
        const auto tauSeconds = juce::jmax (1.0e-4f, milliseconds * 0.001f);
        return std::exp (-1.0f / (tauSeconds * static_cast<float> (sampleRate)));
    };

    attackCoefficient = coefficientFor (duckAttackMs);
    releaseCoefficient = coefficientFor (duckReleaseMs);
}

void WetChain::setLowCutHz (float newLowCutHz)
{
    lastLowCutHz = newLowCutHz;
    lowCutSmoothed.setTargetValue (newLowCutHz);
}

void WetChain::setHighCutHz (float newHighCutHz)
{
    lastHighCutHz = newHighCutHz;
    highCutSmoothed.setTargetValue (newHighCutHz);
}

void WetChain::setDuckAmountPercent (float newDuckAmountPercent)
{
    duckAmount01 = juce::jlimit (0.0f, 1.0f, newDuckAmountPercent * 0.01f);
    duckAmountSmoothed.setTargetValue (duckAmount01);
}

void WetChain::setDuckAttackMs (float newAttackMs)
{
    if (! juce::approximatelyEqual (newAttackMs, duckAttackMs))
    {
        duckAttackMs = newAttackMs;
        updateEnvelopeCoefficients();
    }
}

void WetChain::setDuckReleaseMs (float newReleaseMs)
{
    if (! juce::approximatelyEqual (newReleaseMs, duckReleaseMs))
    {
        duckReleaseMs = newReleaseMs;
        updateEnvelopeCoefficients();
    }
}

bool WetChain::isHardBypassed() const noexcept
{
    const auto lowCutBypassed = lowCutSmoothed.getTargetValue() <= lowCutBypassHz
                                 && lowCutSmoothed.getCurrentValue() <= lowCutBypassHz;
    const auto highCutBypassed = highCutSmoothed.getTargetValue() >= highCutBypassHz
                                  && highCutSmoothed.getCurrentValue() >= highCutBypassHz;
    const auto duckBypassed = duckAmount01 <= 0.0f
                               && duckAmountSmoothed.getCurrentValue() <= 0.0f;

    return lowCutBypassed && highCutBypassed && duckBypassed;
}

void WetChain::process (juce::dsp::AudioBlock<float>& wet, const float* dryMono, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const auto numChannels = static_cast<int> (wet.getNumChannels());

    if (numChannels <= 0)
        return;

    const auto nyquistLimit = static_cast<float> (sampleRate * 0.5) * 0.95f;

    //==========================================================================
    // Low Cut / High Cut. Each is skipped entirely - not run with a flat
    // response - whenever both its target and its current smoothed value sit
    // at the bypass end of the range. That is the neutrality guarantee: at
    // the shipped defaults these two branches are never entered at all.
    const auto lowCutActive = ! (lowCutSmoothed.getTargetValue() <= lowCutBypassHz
                                  && lowCutSmoothed.getCurrentValue() <= lowCutBypassHz);
    const auto highCutActive = ! (highCutSmoothed.getTargetValue() >= highCutBypassHz
                                   && highCutSmoothed.getCurrentValue() >= highCutBypassHz);

    if (lowCutActive || highCutActive)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            // Cutoffs are smoothed per sample (brief 3.4), but the prewarp
            // (a tan() call) is only recomputed while a smoother is actually
            // ramping - in steady state both filters run on static
            // coefficients, exactly as a non-smoothed implementation would.
            if (lowCutActive)
            {
                const auto cutoff = lowCutSmoothed.getNextValue();

                if (lowCutSmoothed.isSmoothing() || i == 0)
                    lowCutFilter.setCutoffFrequency (juce::jmin (cutoff, nyquistLimit));
            }

            if (highCutActive)
            {
                const auto cutoff = highCutSmoothed.getNextValue();

                if (highCutSmoothed.isSmoothing() || i == 0)
                    highCutFilter.setCutoffFrequency (juce::jmin (cutoff, nyquistLimit));
            }

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = wet.getChannelPointer (static_cast<size_t> (channel));
                auto sample = data[i];

                if (lowCutActive)
                    sample = lowCutFilter.processSample (channel, sample);

                if (highCutActive)
                    sample = highCutFilter.processSample (channel, sample);

                data[i] = sample;
            }
        }
    }
    else
    {
        // Keep the smoothers' sample clocks advancing even while bypassed so
        // that a later un-bypass starts its ramp from the right place.
        lowCutSmoothed.skip (numSamples);
        highCutSmoothed.skip (numSamples);
    }

    //==========================================================================
    // Ducker. The envelope follower keeps running even at Duck 0% (so
    // engaging the knob mid-signal doesn't start from a cold envelope), but
    // the gain multiply itself is skipped whenever the commanded amount is 0
    // and the smoother has already settled at exactly 1.0 - again a literal
    // no-op, not a multiply by 1.
    if (dryMono != nullptr)
    {
        // At Duck 0% (target and current both zero) the gain is exactly 1
        // for every sample, so the multiply is skipped outright rather than
        // performed with a unity factor - the bit-identical-bypass guarantee.
        const auto duckActive = ! (duckAmountSmoothed.getTargetValue() <= 0.0f
                                    && duckAmountSmoothed.getCurrentValue() <= 0.0f);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto rectified = std::abs (dryMono[i]);
            const auto coefficient = rectified > envelope ? attackCoefficient : releaseCoefficient;
            envelope = coefficient * envelope + (1.0f - coefficient) * rectified;

            if (! duckActive)
                continue;

            const auto normalised = juce::jmin (1.0f, envelope / duckReferenceEnvelope);
            const auto gain = 1.0f - duckAmountSmoothed.getNextValue() * normalised;

            for (int channel = 0; channel < numChannels; ++channel)
                wet.getChannelPointer (static_cast<size_t> (channel))[i] *= gain;
        }
    }
}
