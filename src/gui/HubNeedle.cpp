#include "HubNeedle.h"

#include <cmath>

namespace
{
    struct Tick
    {
        float db;
        float deg;
    };

    // The alchemie moon-dial has NO printed numerals (unlike the tubecomp
    // VU dial this component was copied from, which has a 9-point tick
    // table calibrated against printed dB numbers) - only two extreme tick
    // glyphs bounding a continuous arc of lunar-phase/planetary symbols (see
    // brand/mocks/alchemie/master-01-base.png). Per the M3 GUI briefing,
    // the sweep range is "the arc between the extreme tick glyphs",
    // measured directly against the master by overlaying a polar protractor
    // grid on brand/mocks/alchemie/master-01-base.png centred on the dial's
    // own measured centre (layout-manifest.json's "mainMeter": cx=687.6,
    // cy=286.5, r=184.7) and visually reading off where the last engraved
    // tick dash on each side sits before the dial face disappears behind
    // the baked hinge-cover dome at the bottom: -130deg on the left (just
    // past the Mars glyph) to +130deg on the right (just past the Jupiter
    // glyph), symmetric, clockwise from straight-up. See
    // docs/gui-mapping.md's "needle sweep measurement" section for the
    // annotated crop images used for this measurement.
    //
    // With only two measured points the piecewise-linear interpolation
    // below (unchanged from aureate's own HubNeedle.cpp) degenerates to a
    // single linear ramp - this is intentional, not a placeholder: there
    // are no intermediate numerals to fit additional points to. The dB
    // range (-60..0 dBFS) is this design's own convention, not a measured
    // quantity - Requiem's output/wet level meter has no printed dB scale
    // to calibrate against, so -60 dBFS (a practical noise-floor-adjacent
    // "silence" reference) rests the needle at the dial's counter-clockwise
    // extreme and 0 dBFS (full-scale) rests it at the clockwise extreme.
    constexpr std::array<Tick, 2> ticks {
        Tick { -60.0f, -130.0f },
        Tick { 0.0f, 130.0f },
    };
}

namespace basilica::gui
{
    HubNeedle::HubNeedle (Assets assetsIn, juce::String accessibleTitle,
                          float pivotXFractionIn, float pivotYFractionIn, float spriteSizeFractionIn,
                          float bakedAngleDegIn)
        : assets (std::move (assetsIn)), title (std::move (accessibleTitle)),
          pivotXFraction (pivotXFractionIn), pivotYFraction (pivotYFractionIn),
          spriteSizeFraction (spriteSizeFractionIn), bakedAngleDeg (bakedAngleDegIn)
    {
        setTitle (title);
        setDescription (title);

        // Pure display - never steals mouse events from controls that may
        // sit under (or within the bounding box of) this component.
        setInterceptsMouseClicks (false, false);
    }

    HubNeedle::~HubNeedle() = default;

    float HubNeedle::tickAngleDegreesForDb (float db) noexcept
    {
        if (db <= ticks.front().db)
            return ticks.front().deg;

        if (db >= ticks.back().db)
            return ticks.back().deg;

        for (size_t i = 1; i < ticks.size(); ++i)
        {
            if (db <= ticks[i].db)
            {
                const auto& lo = ticks[i - 1];
                const auto& hi = ticks[i];
                const auto span = hi.db - lo.db;
                const auto t = span > 0.0f ? (db - lo.db) / span : 0.0f;
                return lo.deg + t * (hi.deg - lo.deg);
            }
        }

        return ticks.back().deg;
    }

    float HubNeedle::stepBallistics (float currentSmoothed, float target, float dtSeconds, float tauSeconds) noexcept
    {
        if (tauSeconds <= 0.0f || dtSeconds <= 0.0f)
            return target;

        const auto alpha = 1.0f - std::exp (-dtSeconds / tauSeconds);
        return currentSmoothed + (target - currentSmoothed) * alpha;
    }

    void HubNeedle::tick (float dtSeconds) noexcept
    {
        const auto target = targetDb.load (std::memory_order_relaxed);
        const auto next = stepBallistics (smoothedDb, target, dtSeconds, ballisticsTauSeconds);

        if (! juce::approximatelyEqual (next, smoothedDb))
        {
            smoothedDb = next;
            repaint();
        }
    }

    void HubNeedle::setImmediateDbForPreview (float db) noexcept
    {
        targetDb.store (db, std::memory_order_relaxed);
        smoothedDb = db;
        repaint();
    }

    void HubNeedle::paint (juce::Graphics& g)
    {
        if (! assets.needleSprite.isValid())
            return;

        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

        const auto bounds = getLocalBounds().toFloat();
        const auto pivotX = bounds.getWidth() * pivotXFraction;
        const auto pivotY = bounds.getHeight() * pivotYFraction;

        const auto spriteDrawSize = spriteSizeFraction * juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto spriteScale = spriteDrawSize / (float) assets.needleSprite.getWidth();

        const auto targetDeg = tickAngleDegreesForDb (smoothedDb);

        // CRITICAL: rotationToApply = targetDeg - bakedAngleDeg. The sprite's
        // own rod already sits at bakedAngleDeg (its pose in the master
        // render it was cut from) - drawing it with targetDeg's own value as
        // the rotation would double-apply that baked pose. See HubNeedle.h's
        // top-of-file docs.
        const auto rotationToApplyDeg = targetDeg - bakedAngleDeg;
        const auto rotationRadians = juce::degreesToRadians (rotationToApplyDeg);

        const auto imageHalfW = 0.5f * (float) assets.needleSprite.getWidth();
        const auto imageHalfH = 0.5f * (float) assets.needleSprite.getHeight();

        const auto transform = juce::AffineTransform::translation (-imageHalfW, -imageHalfH)
                                    .scaled (spriteScale)
                                    .rotated (rotationRadians)
                                    .translated (pivotX, pivotY);

        g.drawImageTransformed (assets.needleSprite, transform, false);
    }

    // Read-only text value interface exposing the current ballistic-smoothed
    // reading, mirroring basilica-audio/silentium's AnalogMeter::
    // MeterValueInterface (JUCE 8.0.14's own juce::AccessibilityTextValueInterface
    // shape).
    class HubNeedle::ValueInterface final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit ValueInterface (const HubNeedle& ownerIn) noexcept : owner (ownerIn) {}

        bool isReadOnly() const override { return true; }

        juce::String getCurrentValueAsString() const override
        {
            return juce::String (owner.smoothedDb, 1) + " dB";
        }

        void setValueAsString (const juce::String&) override {}

    private:
        const HubNeedle& owner;
    };

    std::unique_ptr<juce::AccessibilityHandler> HubNeedle::createAccessibilityHandler()
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this,
            juce::AccessibilityRole::label,
            juce::AccessibilityActions {},
            juce::AccessibilityHandler::Interfaces { std::make_unique<ValueInterface> (*this) });
    }
}
