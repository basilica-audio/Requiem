#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"

#include <BinaryData.h>

#include <catch2/catch_test_macros.hpp>

// Typography-pass proof (suite typo phase, owner decision 2026-07-26): the
// alchemie master's crystal knobs and bone buttons carry no function
// names - the silver DECAY/PRE-DELAY/MIX/DAMPING/SIZE caption row in the
// plate's bottom margin and the FREEZE/IMPULSE button captions are a live
// JUCE text layer (src/gui/PlateTypography.h, drawn last in
// PluginEditor::paint()). Proofs: (1) the silver lettering brightens each
// caption's box where the raw master's near-black aubergine has almost no
// bright pixels; (2) a flat-ground unit render of the shared glyph draw
// path; (3) a layout invariant keeping the caption row clear of the knob
// hit-areas and inside the plate.
namespace
{
    float brightFractionIn (const juce::Image& image, juce::Rectangle<int> area, int threshold)
    {
        int bright = 0, total = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
        {
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto c = image.getPixelAt (x, y);
                const auto lum = (int) std::lround (0.299f * c.getRed() + 0.587f * c.getGreen() + 0.114f * c.getBlue());

                ++total;

                if (lum > threshold)
                    ++bright;
            }
        }

        return total > 0 ? (float) bright / (float) total : 0.0f;
    }

    juce::Rectangle<int> toSnapshotRect (juce::Rectangle<float> plateLocal1x)
    {
        return plateLocal1x
            .translated (0.0f, (float) (rqm::layout::topStripHeight1x + rqm::layout::topStripGap1x))
            .getSmallestIntegerContainer();
    }

    juce::Rectangle<int> toMasterRect (juce::Rectangle<float> plateLocal1x)
    {
        const auto toMaster = (float) rqm::layout::masterCanvasWidthPx / (float) rqm::layout::plateWidth1x;

        return juce::Rectangle<float> (plateLocal1x.getX() * toMaster, plateLocal1x.getY() * toMaster,
                                       plateLocal1x.getWidth() * toMaster, plateLocal1x.getHeight() * toMaster)
            .getSmallestIntegerContainer();
    }
}

TEST_CASE ("Silver knob and button captions brighten the dark aubergine plate", "[gui][typography]")
{
    using namespace rqm::layout;

    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    RequiemAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);

    const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f, juce::SoftwareImageType {});
    REQUIRE (snapshot.isValid());

    const auto master = juce::ImageCache::getFromMemory (BinaryData::master_alchemie_png,
                                                         BinaryData::master_alchemie_pngSize);
    REQUIRE (master.isValid());

    struct CaptionCore
    {
        const char* name;
        int cx, cy, w; // text-core boxes (tighter than the production draw
                       // boxes, so a baked engraving grazing a box edge
                       // cannot dilute the master-side reading)
    };

    const CaptionCore cores[] = {
        { "DECAY", knobSlots1x[0].cx1x, knobCaptionCy1x, 48 },
        { "PRE-DELAY", knobSlots1x[1].cx1x, knobCaptionCy1x, 64 },
        { "MIX", knobSlots1x[2].cx1x, knobCaptionCy1x, 30 },
        { "DAMPING", knobSlots1x[3].cx1x, knobCaptionCy1x, 56 },
        { "SIZE", knobSlots1x[4].cx1x, knobCaptionCy1x, 36 },
        { "FREEZE", buttonLeft1x.cx1x, buttonLeft1x.cy1x + buttonCaptionOffsetY1x + 5, 44 },
        { "IMPULSE", buttonRight1x.cx1x, buttonRight1x.cy1x + buttonCaptionOffsetY1x + 5, 48 },
    };

    for (const auto& core : cores)
    {
        INFO (core.name);

        const juce::Rectangle<float> box1x ((float) core.cx - (float) core.w * 0.5f,
                                            (float) core.cy - 6.0f, (float) core.w, 12.0f);

        // Silver lettering (~200 luminance at coverage) against the
        // near-black aubergine: at threshold 140 the raw master's boxes
        // measure 0-0.9% bright, the lettered snapshot 3.7-8.7%
        // (calibrated against the real render during this pass).
        const auto snapshotBright = brightFractionIn (snapshot, toSnapshotRect (box1x), 140);
        const auto masterBright = brightFractionIn (master, toMasterRect (box1x), 140);

        CHECK (snapshotBright > masterBright + 0.015f);
    }
}

TEST_CASE ("PlateTypography renders glyphs and its offset pass on a flat ground", "[gui][typography]")
{
    basilica::gui::PlateTypography typography (BinaryData::EBGaramondRegular_ttf,
                                               (int) BinaryData::EBGaramondRegular_ttfSize,
                                               BinaryData::EBGaramondSemiBold_ttf,
                                               (int) BinaryData::EBGaramondSemiBold_ttfSize);

    const juce::Colour ground (0xff262029); // aubergine, luminance ~34

    juce::Image canvas (juce::Image::RGB, 160, 24, true);
    {
        juce::Graphics g (canvas);
        g.fillAll (ground);

        const basilica::gui::EngravedTextStyle style {
            juce::Colour (0xe6cfd4de), juce::Colour (0x90000000), 11.5f, 0.16f, true
        };

        typography.drawEngraved (g, "DAMPING", canvas.getBounds().toFloat(), 1.0f, style);
    }

    int silverPixels = 0;

    for (int y = 0; y < canvas.getHeight(); ++y)
    {
        for (int x = 0; x < canvas.getWidth(); ++x)
        {
            const auto c = canvas.getPixelAt (x, y);
            const auto lum = 0.299f * c.getRed() + 0.587f * c.getGreen() + 0.114f * c.getBlue();

            if (lum > 140.0f)
                ++silverPixels;
        }
    }

    // 7 semibold capitals at 11.5px leave a solid body of silver pixels.
    // (No shadow-pixel check here: the ground is itself nearly black, so
    // the dark offset pass is legitimately invisible against it.) The
    // floor is deliberately cross-platform-loose: the Windows glyph
    // rasterizer renders visibly thinner coverage than macOS for the same
    // face/height (sibling CI runs 33026829812/33028029166 measured
    // roughly half the macOS coverage) - the check only needs to fail
    // loudly for MISSING text.
    CHECK (silverPixels > 25);
}

TEST_CASE ("Caption row stays clear of the knob hit-areas and inside the plate", "[gui][typography]")
{
    using namespace rqm::layout;

    for (const auto& slot : knobSlots1x)
    {
        const auto sliderBottom = knobRowY1x + slot.diameter1x / 2;
        CHECK (knobCaptionCy1x - knobCaptionHeight1x / 2 > sliderBottom);
    }

    CHECK (knobCaptionCy1x + knobCaptionHeight1x / 2 < plateHeight1x);
}
