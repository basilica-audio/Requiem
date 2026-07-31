#include "AdditiveGlow.h"

#include <cmath>

namespace
{
    // Builds one composited frame: offPlate crop + rgb*(alphaFraction)*
    // additiveGain*gain, clamped per-channel to [0,255]. Shared by both the
    // t=1 "lit" frame (gain=1.0) and the optional "overshoot" frame
    // (gain=overshootGain>1.0) - message-thread-only per-pixel work, done
    // once at construction, never touched from paint()/the audio thread.
    juce::Image buildCompositedFrame (const juce::Image& offPlateImage, const juce::Image& glowImage,
                                      juce::Point<int> glowOffsetInMaster, float additiveGain, float gain)
    {
        const auto w = glowImage.getWidth();
        const auto h = glowImage.getHeight();

        juce::Image out (juce::Image::ARGB, w, h, false);
        juce::Image::BitmapData dst (out, juce::Image::BitmapData::writeOnly);

        const auto addChannel = [additiveGain, gain] (juce::uint8 offChannel, juce::uint8 glowChannel, float alphaFraction)
        {
            const auto value = (float) offChannel + (float) glowChannel * alphaFraction * additiveGain * gain;
            return (juce::uint8) juce::jlimit (0.0f, 255.0f, value);
        };

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const auto glow = glowImage.getPixelAt (x, y);
                const auto alphaFraction = (float) glow.getAlpha() / 255.0f;

                const auto srcX = juce::jlimit (0, offPlateImage.getWidth() - 1, glowOffsetInMaster.x + x);
                const auto srcY = juce::jlimit (0, offPlateImage.getHeight() - 1, glowOffsetInMaster.y + y);
                const auto off = offPlateImage.getPixelAt (srcX, srcY);

                const auto outR = addChannel (off.getRed(), glow.getRed(), alphaFraction);
                const auto outG = addChannel (off.getGreen(), glow.getGreen(), alphaFraction);
                const auto outB = addChannel (off.getBlue(), glow.getBlue(), alphaFraction);

                dst.setPixelColour (x, y, juce::Colour::fromRGB (outR, outG, outB));
            }
        }

        return out;
    }
}

namespace basilica::gui
{
    AdditiveGlow::AdditiveGlow (const juce::Image& offPlateImage, const juce::Image& glowImage,
                                juce::Point<int> glowOffsetInMaster, float additiveGain, float overshootGain)
        : glowOffset (glowOffsetInMaster), overshootGainValue (juce::jmax (1.0f, overshootGain))
    {
        if (! offPlateImage.isValid() || ! glowImage.isValid())
            return;

        glowWidth = glowImage.getWidth();
        glowHeight = glowImage.getHeight();

        litImage = buildCompositedFrame (offPlateImage, glowImage, glowOffsetInMaster, additiveGain, 1.0f);

        if (overshootGainValue > 1.0f)
            overshootImage = buildCompositedFrame (offPlateImage, glowImage, glowOffsetInMaster, additiveGain, overshootGainValue);
    }

    void AdditiveGlow::drawRing (juce::Graphics& g, juce::Rectangle<float> destRectOnScreen, float t) const
    {
        if (! isValid())
            return;

        const auto clampedT = juce::jlimit (0.0f, overshootGainValue, t);

        juce::Graphics::ScopedSaveState saveState (g);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        if (clampedT <= 1.0f)
        {
            if (clampedT <= 0.001f)
                return;

            g.setOpacity (clampedT);
            g.drawImage (litImage, destRectOnScreen, juce::RectanglePlacement::stretchToFit, false);
            return;
        }

        // t > 1: draw the t=1 frame fully opaque (replacing whatever the
        // caller drew underneath with the exact same pixels the opaque=1.0
        // branch above would have produced), then cross-blend the
        // precomputed overshoot frame on top - see class docs for why this
        // never calls setOpacity() outside [0,1].
        g.setOpacity (1.0f);
        g.drawImage (litImage, destRectOnScreen, juce::RectanglePlacement::stretchToFit, false);

        if (! overshootImage.isValid() || overshootGainValue <= 1.0f)
            return;

        const auto frac = juce::jlimit (0.0f, 1.0f, (clampedT - 1.0f) / (overshootGainValue - 1.0f));

        if (frac <= 0.001f)
            return;

        g.setOpacity (frac);
        g.drawImage (overshootImage, destRectOnScreen, juce::RectanglePlacement::stretchToFit, false);
    }

    void AdditiveGlow::drawWedge (juce::Graphics& g, juce::Rectangle<float> destRectOnScreen,
                                  juce::Point<float> centreInMaster, float startAngleDeg, float endAngleDeg,
                                  float proportion) const
    {
        if (! isValid())
            return;

        const auto clampedProportion = juce::jlimit (0.0f, 1.0f, proportion);

        if (clampedProportion <= 0.0001f)
            return;

        const auto sweep = endAngleDeg - startAngleDeg;
        const auto currentEndDeg = startAngleDeg + sweep * clampedProportion;

        // Map centreInMaster (off-plate/master pixel space) into
        // destRectOnScreen's own coordinate space via this glow zone's own
        // measured footprint (glowOffset/glowWidth/glowHeight, the same
        // rect glows.json's offsetX/offsetY/width/height describe).
        const auto scaleX = glowWidth > 0 ? destRectOnScreen.getWidth() / (float) glowWidth : 1.0f;
        const auto scaleY = glowHeight > 0 ? destRectOnScreen.getHeight() / (float) glowHeight : 1.0f;

        const auto centreScreenX = destRectOnScreen.getX() + (centreInMaster.x - (float) glowOffset.x) * scaleX;
        const auto centreScreenY = destRectOnScreen.getY() + (centreInMaster.y - (float) glowOffset.y) * scaleY;

        // Radius generous enough to cover the whole glow footprint from its
        // own centre - the sprite's own alpha (zero outside its measured
        // ring band) provides the real radial masking, so this clip only
        // needs to bound the ANGULAR sector, not fit a tight radius.
        const auto radius = juce::jmax (destRectOnScreen.getWidth(), destRectOnScreen.getHeight());

        juce::Path wedge;
        wedge.addPieSegment (centreScreenX - radius, centreScreenY - radius, radius * 2.0f, radius * 2.0f,
                             juce::degreesToRadians (startAngleDeg), juce::degreesToRadians (currentEndDeg), 0.0f);

        juce::Graphics::ScopedSaveState saveState (g);

        if (! g.reduceClipRegion (wedge))
            return; // zero-size intersection (proportion effectively 0) - nothing to draw

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);
        g.drawImage (litImage, destRectOnScreen, juce::RectanglePlacement::stretchToFit, false);
    }
}
