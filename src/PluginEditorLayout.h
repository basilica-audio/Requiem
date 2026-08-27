#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

// Requiem's own @1x faceplate/control-bay geometry table for the M3
// photoreal "alchemie" GUI - lives in its own header (basilica-audio/
// aureate's PluginEditorLayout.h convention, copied here), so
// tests/gui/EditorLayoutTests.cpp can assert layout invariants directly
// against the SAME numbers PluginEditor.cpp actually lays components out
// with.
//
// Every constant below is derived from the alchemie design's own measured
// provenance (repo-relative to the suite root, one level above this repo):
//   - brand/mocks/alchemie/layout-manifest.json - plate/knob/button/window
//     geometry, measured against master-01-base.png.
//   - brand/mocks/alchemie/components/needle.json - the needle hub pivot
//     (2026-07-31 corrected values: 695.00/383.00 master px - see
//     src/gui/HubNeedle.h's "needle pivot verification" docs).
//   - brand/mocks/alchemie/components/glows.json - the 4 knob-ring and 1
//     bezel additive-glow sprites' own canvas offset/size/centre/angle
//     within the master.
//   - the needle sweep range (-130..+130deg) was independently MEASURED for
//     this pass (not taken from any JSON field - the design has no printed
//     numerals to calibrate a tick table against) by overlaying a polar
//     protractor grid on master-01-base.png centred on the dial's own
//     measured centre (layout-manifest.json's "mainMeter") and reading off
//     the last engraved tick dash on each side before the dial face
//     disappears behind the baked hinge-cover dome - see
//     docs/gui-mapping.md's "needle sweep measurement" section for the
//     annotated crop images.
//
// scale = plateWidth1x / masterCanvasWidthPx, the SAME uniform-canvas-scale
// convention aureate's own layout table uses (i.e. the plate rect draws the
// design's FULL master canvas, background dressing included - it is not
// cropped to layout-manifest.json's own inner "plateBounds" sub-rect,
// which only describes where the physical panel edges sit within that
// canvas, not a crop boundary).
//
// juce::Point's constructors ARE constexpr in this pinned JUCE 8.0.14
// checkout (verified directly against modules/juce_graphics/geometry/
// juce_Point.h - contradicts an over-cautious note in aureate's own copy of
// this file), but juce::Rectangle's are NOT (juce_Rectangle.h's 4-argument
// constructor omits the constexpr specifier) - Rectangle-shaped geometry
// below is therefore stored as plain (x,y,w,h) int/float quads rather than
// juce::Rectangle members, so every table in this file stays a fully
// literal, constexpr-constructible aggregate.
namespace rqm::layout
{
    constexpr int masterCanvasWidthPx = 1376;
    constexpr int masterCanvasHeightPx = 768;

    constexpr int plateWidth1x = 900;
    constexpr int plateHeight1x = 502; // masterCanvasHeightPx scaled by the same factor as plateWidth1x

    //==========================================================================
    // Knob body hit-areas: layout-manifest.json's "knobs" array, index 1-5,
    // reading order left-to-right - ALL FIVE knobs sit on a single shared
    // bottom row (unlike aureate's tubecomp two-row grid). cx1x/diameter1x
    // convert each knob's own measured master-px centre/radius via the
    // scale above; the row is snapped to its own shared mean Y
    // (layout-manifest.json's "rowYs": mean_cy=586.6, max_deviation_px=1.6 -
    // well within a ~49-59px knob radius, so snapping is imperceptible and
    // keeps the row-alignment invariant structurally guaranteed).
    struct KnobSlot1x
    {
        int cx1x;
        int diameter1x;
    };

    constexpr int knobRowY1x = 384;

    constexpr std::array<KnobSlot1x, 5> knobSlots1x {{
        { 161, 64 }, // slot 0 (leftmost, layout-manifest knob index 1)
        { 300, 65 }, // slot 1 (index 2)
        { 445, 77 }, // slot 2 (index 3 - CENTRE, octagonal-faceted and larger; radius is a circumscribed-circle approximation, see layout-manifest.json's own note)
        { 592, 76 }, // slot 3 (index 4)
        { 734, 72 }, // slot 4 (rightmost, index 5)
    }};

    // Index into knobSlots1x that carries no ring glow sprite (the larger,
    // octagonal-faceted centre knob) - see docs/gui-mapping.md for this
    // knob's own feedback decision.
    constexpr int centreKnobSlotIndex = 2;

    //==========================================================================
    // Buttons: layout-manifest.json's "buttons" array.
    struct ButtonSlot1x
    {
        int cx1x, cy1x, diameter1x;
    };

    constexpr ButtonSlot1x buttonLeft1x { 117, 306, 34 };
    constexpr ButtonSlot1x buttonRight1x { 782, 306, 37 };

    //==========================================================================
    // Windows: layout-manifest.json's "rectFeatures" - reserved for a future
    // IR/decay display, stay dark/inert this revision (see
    // docs/gui-mapping.md). Kept here purely so a future pass has the
    // measured geometry on hand without re-deriving it; (x,y,w,h) @1x.
    constexpr int windowLeft1x[4] { 108, 105, 219, 144 };
    constexpr int windowRight1x[4] { 582, 101, 209, 152 };

    //==========================================================================
    // Needle/meter bay: HubNeedle's own component bounds, centred on the
    // measured hub pivot. componentSizeMasterPx (448 master px) is the
    // needle sprite's own canvas (440) plus an 8px rounding-safety margin -
    // the sprite's own extraction pipeline already sized a 12px marginPx
    // into that 440 canvas for full-rotation clipping safety
    // (components/needle.json), so this only needs to cover rounding, not
    // clipping.
    constexpr juce::Point<float> needlePivotMasterPx { 695.0f, 383.0f };
    constexpr int meterComponentSize1x = 293;
    constexpr juce::Point<int> meterTopLeft1x { 308, 104 };
    constexpr float needleSpritePivotFraction = 0.5f; // needle.json spriteIsPivotCentred:true, pivotXFrac/pivotYFrac=0.5
    constexpr float needleSpriteSizeFraction = 440.0f / 448.0f;
    constexpr float needleBakedAngleDeg = 0.0f; // needle.json restAngleDeg

    // Needle sweep range (see this file's top-of-file "needle sweep
    // measurement" docs) and the dB range it's mapped from (this design's
    // own convention - Requiem's output/wet-level meter has no printed dB
    // scale to calibrate against; see src/gui/HubNeedle.cpp's `ticks`
    // table, which duplicates these same four numbers).
    constexpr float needleSweepMinDeg = -130.0f;
    constexpr float needleSweepMaxDeg = 130.0f;
    constexpr float needleLevelFloorDb = -60.0f;
    constexpr float needleLevelCeilingDb = 0.0f;

    //==========================================================================
    // Bezel glow zone (glows.json's "glow-bezel.png" sprite footprint) - the
    // big ring around the moon-dial the owner asked to "glow once" at
    // startup (signature behaviour #2).
    constexpr int bezelGlowZoneMasterPx[4] { 506, 125, 371, 314 };
    constexpr int bezelGlowZone1x[4] { 331, 82, 243, 205 };
    constexpr juce::Point<float> bezelGlowCentreMasterPx { 691.1f, 307.09f };
    constexpr float bezelGlowStartAngleDeg = -122.5f;
    constexpr float bezelGlowEndAngleDeg = 121.5f;

    // Startup bezel-glow animation timing (see PluginEditor.cpp's
    // easeOutBackWithOvershoot()). bezelOvershootPeakT is that easing
    // function's own analytic peak value at backC1=2.2 (~115.4%, matching
    // the owner's "~115%" brief) - verified numerically in
    // tests/gui/EditorSnapshotTests.cpp.
    constexpr float bezelStartupDurationSeconds = 1.2f;
    constexpr float bezelOvershootPeakT = 1.1541f;

    //==========================================================================
    // Knob ring glow zones (glows.json's "sprites" array). NOTE these are
    // NOT the same circle as the knob body above: the ring halo sits
    // outside the crystal (glows.json's own radius ~66-72 master px vs the
    // knob body's own measured ~49-58 master px radius), so this table
    // carries its own independent centre/angle geometry, cross-checked
    // against knobSlots1x in docs/gui-mapping.md ("ring-to-knob mapping").
    // Slot 2 (the centre knob) intentionally has no entry here - see
    // centreKnobSlotIndex above.
    struct KnobRingZone
    {
        int knobSlotIndex; // index into knobSlots1x
        int zoneOffsetXMasterPx, zoneOffsetYMasterPx, zoneWidthMasterPx, zoneHeightMasterPx;
        int zoneX1x, zoneY1x, zoneW1x, zoneH1x;
        float centreXMasterPx, centreYMasterPx;
        float startAngleDeg, endAngleDeg;
    };

    constexpr std::array<KnobRingZone, 4> knobRingZones {{
        { 0, 176, 514, 167, 138, 115, 336, 109, 90, 255.38f, 592.98f, -126.0f, 125.5f },
        { 1, 370, 484, 213, 182, 242, 317, 139, 119, 472.06f, 595.41f, -128.0f, 130.0f },
        { 3, 790, 483, 227, 191, 517, 316, 148, 125, 902.57f, 595.04f, -127.5f, 132.0f },
        { 4, 1008, 490, 219, 183, 659, 320, 143, 120, 1118.95f, 594.29f, -124.5f, 126.5f },
    }};

    constexpr int topStripHeight1x = 32;
    constexpr int topStripGap1x = 6;
    //==========================================================================
    // Typography pass (suite typo phase, owner decision 2026-07-26: text is
    // never baked into the AI master - lettering is set locally as a sharp
    // JUCE text layer, see src/gui/PlateTypography.h and
    // docs/gui-mapping.md's typography section). The alchemie design's
    // crystal knobs carry no function names (only baked glyph garbles at
    // their flanks); this pass adds one silver caption per knob in the
    // plate's bottom margin - the clean dark band UNDER the engraved
    // scroll border (master y ~707..724; the band directly under the knob
    // bases is blocked by the border's central fleur ornament) - plus one
    // caption under each of the two bone buttons. Silver lettering (not
    // gold): every baked engraving on this aubergine plate is silver, and
    // the captions must read as part of that same engraving system.
    constexpr int knobCaptionCy1x = 468; // bottom-margin caption row centre
    constexpr int knobCaptionWidth1x = 100;
    constexpr int knobCaptionHeight1x = 12;

    constexpr int buttonCaptionOffsetY1x = 17; // button-centre -> caption-box top (just clear of the button's lower edge)
    constexpr int buttonCaptionWidth1x = 64;
    constexpr int buttonCaptionHeight1x = 11;

    constexpr int scaleButtonWidth1x = 64;

    constexpr int baseEditorWidth = plateWidth1x;
    constexpr int baseEditorHeight = topStripHeight1x + topStripGap1x + plateHeight1x;

    constexpr std::array<float, 3> scaleSteps { 1.0f, 1.5f, 2.0f };
}
