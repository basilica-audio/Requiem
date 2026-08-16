#include "PluginEditorLayout.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Layout-invariant tests for the M3 photoreal "alchemie" GUI - the alchemie
// design's own layout-manifest.json's "rowYs" field is the source of truth
// for the knob row-Y snap (see PluginEditorLayout.h's top-of-file docs), not
// each individual knob's own slightly-deviating measured centre.
TEST_CASE ("All 5 knobs sit on a single shared row with no overlap", "[gui][layout]")
{
    using namespace rqm::layout;

    CHECK (knobSlots1x.size() == 5);

    for (const auto& slot : knobSlots1x)
        CHECK (slot.diameter1x > 0);

    // No two knob hit-areas should overlap each other, given the shared row
    // Y and each knob's own diameter.
    for (size_t i = 0; i < knobSlots1x.size(); ++i)
    {
        const juce::Rectangle<int> a { knobSlots1x[i].cx1x - knobSlots1x[i].diameter1x / 2,
                                       knobRowY1x - knobSlots1x[i].diameter1x / 2,
                                       knobSlots1x[i].diameter1x, knobSlots1x[i].diameter1x };

        for (size_t j = i + 1; j < knobSlots1x.size(); ++j)
        {
            const juce::Rectangle<int> b { knobSlots1x[j].cx1x - knobSlots1x[j].diameter1x / 2,
                                           knobRowY1x - knobSlots1x[j].diameter1x / 2,
                                           knobSlots1x[j].diameter1x, knobSlots1x[j].diameter1x };
            CHECK_FALSE (a.intersects (b));
        }
    }
}

TEST_CASE ("The needle/meter component and both buttons stay within the plate's own canvas bounds", "[gui][layout]")
{
    using namespace rqm::layout;

    const juce::Rectangle<int> plateCanvas { 0, 0, plateWidth1x, plateHeight1x };

    const juce::Rectangle<int> meterBay { meterTopLeft1x.x, meterTopLeft1x.y, meterComponentSize1x, meterComponentSize1x };
    CHECK (plateCanvas.contains (meterBay));

    const juce::Rectangle<int> left { buttonLeft1x.cx1x - buttonLeft1x.diameter1x / 2,
                                      buttonLeft1x.cy1x - buttonLeft1x.diameter1x / 2,
                                      buttonLeft1x.diameter1x, buttonLeft1x.diameter1x };
    const juce::Rectangle<int> right { buttonRight1x.cx1x - buttonRight1x.diameter1x / 2,
                                       buttonRight1x.cy1x - buttonRight1x.diameter1x / 2,
                                       buttonRight1x.diameter1x, buttonRight1x.diameter1x };
    CHECK (plateCanvas.contains (left));
    CHECK (plateCanvas.contains (right));

    const juce::Rectangle<int> bezelZone { bezelGlowZone1x[0], bezelGlowZone1x[1], bezelGlowZone1x[2], bezelGlowZone1x[3] };
    CHECK (plateCanvas.contains (bezelZone));

    for (const auto& zone : knobRingZones)
    {
        const juce::Rectangle<int> ringZone { zone.zoneX1x, zone.zoneY1x, zone.zoneW1x, zone.zoneH1x };
        CHECK (plateCanvas.contains (ringZone));
    }
}

TEST_CASE ("Needle sprite geometry is well-formed", "[gui][layout]")
{
    using namespace rqm::layout;

    CHECK (needleSpritePivotFraction > 0.0f);
    CHECK (needleSpritePivotFraction < 1.0f);
    CHECK (needleSpriteSizeFraction > 0.0f);
    CHECK (needleSpriteSizeFraction < 1.0f); // sprite must not exceed its own component's bounds

    // The pivot must sit strictly inside the meter component's own bounds -
    // a fraction outside [0,1] would mean the needle rotates around a point
    // off the drawn component entirely.
    const juce::Rectangle<int> meterBay { meterTopLeft1x.x, meterTopLeft1x.y, meterComponentSize1x, meterComponentSize1x };
    CHECK (meterBay.contains (juce::roundToInt (needlePivotMasterPx.x * (float) plateWidth1x / (float) masterCanvasWidthPx),
                              juce::roundToInt (needlePivotMasterPx.y * (float) plateWidth1x / (float) masterCanvasWidthPx)));
}

TEST_CASE ("Needle sweep range is symmetric and well-formed", "[gui][layout]")
{
    using namespace rqm::layout;

    CHECK (needleSweepMinDeg < 0.0f);
    CHECK (needleSweepMaxDeg > 0.0f);
    CHECK (needleSweepMaxDeg == Catch::Approx (-needleSweepMinDeg)); // symmetric about straight-up, as measured
    CHECK (needleLevelFloorDb < needleLevelCeilingDb);
}

TEST_CASE ("Knob ring zones map onto valid, distinct knob slots and non-degenerate angular sweeps", "[gui][layout]")
{
    using namespace rqm::layout;

    CHECK (knobRingZones.size() == 4);

    for (const auto& zone : knobRingZones)
    {
        CHECK (zone.knobSlotIndex >= 0);
        CHECK (zone.knobSlotIndex < (int) knobSlots1x.size());
        CHECK (zone.knobSlotIndex != centreKnobSlotIndex); // the centre knob has no ring, by design
        CHECK (zone.zoneW1x > 0);
        CHECK (zone.zoneH1x > 0);
        CHECK (zone.endAngleDeg > zone.startAngleDeg); // clockwise sweep
    }

    // Every outer slot except the centre carries exactly one ring.
    std::array<bool, 5> covered {};
    for (const auto& zone : knobRingZones)
        covered[(size_t) zone.knobSlotIndex] = true;

    for (size_t i = 0; i < covered.size(); ++i)
        CHECK (covered[i] == (i != (size_t) centreKnobSlotIndex));
}

TEST_CASE ("Startup bezel-glow overshoot peak matches the owner's ~115% brief", "[gui][layout]")
{
    using namespace rqm::layout;

    CHECK (bezelOvershootPeakT > 1.0f);
    CHECK (bezelOvershootPeakT == Catch::Approx (1.1541f).margin (0.01f));
    CHECK (bezelStartupDurationSeconds == Catch::Approx (1.2f));
}
