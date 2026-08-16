#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>

// Suite-reusable pivot-centred needle overlay - copied verbatim (class shape,
// rotation maths, ballistics, accessibility handler) from basilica-audio/
// aureate's M3 GUI pilot (src/gui/HubNeedle.{h,cpp}, the "tubecomp" faceplate
// family's needle component), and ADAPTED here for Requiem's own "alchemie"
// faceplate design - see HubNeedle.cpp's `ticks` table docs for exactly what
// changed and why (the tubecomp dial has 9 printed numeral ticks; the
// alchemie moon-dial has no numerals at all, just two extreme tick glyphs
// bounding a continuous symbol arc, so the piecewise table degenerates to a
// simple 2-point linear range rather than a 9-point curve - the
// interpolation code itself is untouched).
//
// The dial FACE (bezel, tick marks, planetary/lunar glyphs) is baked into
// the design's master render (resources/gui/master_alchemie.png) - this
// component draws ONLY the live needle sprite on top of it, rotated about
// the sprite's own measured hub pivot via a live juce::AffineTransform
// (never a pre-rotated frame stack - see components/needle.json's own
// provenance notes for why the master-extraction pipeline deliberately does
// not rotate the sprite to a canonical pose: doing so would resample and
// soften the master's own pixels).
//
// CRITICAL (binding rule, see the M3 GUI briefing): the sprite's pivot is
// the needle's HUB CENTRE, not the visible rod end - components/needle.json's
// own pivotXInMaster/pivotYInMaster fields (695.00, 383.00, the 2026-07-31
// fix) already encode this, and this component's pivotXFraction/
// pivotYFraction constructor parameters must be derived from that same
// point (never the rod end), or the needle base will visibly lift off its
// hub as it rotates. Verified against the master (see docs/gui-mapping.md's
// "needle pivot verification" section): the pivot lands exactly on the dark
// domed hub-cover baked into the master, and the needle sprite's own visible
// content terminates ~34px above the sprite's own canvas centre - i.e. the
// rod's tail is meant to disappear behind that baked dome at every rotation
// angle, not touch the pivot itself.
namespace basilica::gui
{
    class HubNeedle : public juce::Component
    {
    public:
        struct Assets
        {
            // The master-extracted needle sprite (needle_alchemie.png) -
            // PIVOT-CENTRED canvas (pivot sits at the sprite's own exact
            // canvas centre, fraction 0.5/0.5 - see needle.json's
            // pivotXFrac/pivotYFrac and spriteIsPivotCentred:true), so no
            // additional pivot-offset maths is needed when rotating it
            // about its own centre.
            juce::Image needleSprite;
        };

        // pivotXFraction/pivotYFraction: where the needle's hub pivot sits,
        // as a fraction of this component's own local bounds - measured
        // once against the master render (see PluginEditorLayout.h's
        // needlePivotMasterPx docs) and passed in here.
        //
        // spriteSizeFraction: the needle sprite's own drawn diameter, as a
        // fraction of jmin(width,height) of this component's bounds.
        //
        // bakedAngleDegIn: the sprite's own rest pose in the master render
        // it was extracted from (needle.json's restAngleDeg) - rotation
        // applied each frame is (targetAngle - bakedAngleDegIn), NOT
        // targetAngle alone (see paint()'s docs).
        HubNeedle (Assets assetsIn, juce::String accessibleTitle,
                  float pivotXFraction, float pivotYFraction, float spriteSizeFraction,
                  float bakedAngleDegIn);
        ~HubNeedle() override;

        // Thread-safe (plain atomic store): the instantaneous value in dB,
        // written from the audio thread (or the editor's own polling
        // timer). Ballistic smoothing is applied separately, on the GUI
        // thread, so this is real-time safe to call from anywhere.
        void setTargetDb (float newTargetDb) noexcept { targetDb.store (newTargetDb, std::memory_order_relaxed); }

        // Advances the ballistic smoothing by dtSeconds and repaints if the
        // smoothed value changed meaningfully - called from the editor's own
        // timer (see PluginEditor.cpp), NOT owned internally by a
        // juce::Timer on this component, so headless tests can drive it
        // deterministically without a running message loop.
        void tick (float dtSeconds) noexcept;

        // Test/preview-only: seeds both the raw target and the ballistic-
        // smoothed reading to the same value immediately, bypassing the
        // ramp - headless test binaries have no running message loop to
        // pump real ticks through. Normal operation never calls this.
        void setImmediateDbForPreview (float db) noexcept;

        void paint (juce::Graphics& g) override;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

        // One-pole ballistic integration step, exposed as a pure/static
        // function so it is directly unit-testable without a running timer.
        static float stepBallistics (float currentSmoothed, float target, float dtSeconds, float tauSeconds) noexcept;

        // dB -> face-relative rotation angle in degrees. For the alchemie
        // moon-dial (no printed numerals) this is a simple 2-point linear
        // range clamped beyond its ends - see HubNeedle.cpp's `ticks` docs
        // and docs/gui-mapping.md's "needle sweep measurement" section for
        // how the -130/+130deg extremes were measured against the master.
        // Degrees are clockwise from straight-up (12 o'clock).
        static float tickAngleDegreesForDb (float db) noexcept;

        static constexpr float ballisticsTauSeconds = 0.25f;

    private:
        class ValueInterface;

        Assets assets;
        juce::String title;

        std::atomic<float> targetDb { 0.0f };
        float smoothedDb = 0.0f;

        const float pivotXFraction;
        const float pivotYFraction;
        const float spriteSizeFraction;
        const float bakedAngleDeg;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HubNeedle)
    };
}
