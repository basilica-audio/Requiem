#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Requiem's own "alchemie" faceplate design light overlay - NEW for this M3
// GUI (not copied from basilica-audio/aureate's SubtractiveGlow.h). The two
// designs' extraction pipelines diverge structurally, not just cosmetically:
// tubecomp's vent-glow.json diff was taken as (fully-lit - fully-dim), with
// the FULLY-LIT master as the shipped runtime baseline (so the overlay only
// ever DARKENS from that baseline - see SubtractiveGlow.h's own docs).
// alchemie's glows.json diff was taken the other way around and registered
// onto the UNLIT frame (master-03-glows-off.png, THIS design's shipped
// runtime baseline - see the M3 GUI briefing's ASSETS section and
// glows.json's own "registration"/"note" fields), so the overlay only ever
// BRIGHTENS from that baseline:
//
//   frame = clamp(off + rgb*(alpha/255)*additiveGain*t)
//
// off is the runtime baseline (master_alchemie.png, already drawn by the
// caller before this class's draw*() methods run); rgb/alpha come from the
// design's own per-zone glow sprite (glow-knob-N.png/glow-bezel.png); t is
// this call's own intensity. Because rgb/alpha were extracted as EXACTLY
// (lit_registered - unlit) with additiveGain 1.0 (glows.json's own
// top-level field), off + rgb*(alpha/255)*1.0 reconstructs the lit master
// pixel-for-pixel (modulo sub-pixel registration residual) BY CONSTRUCTION -
// the standard 0-255 channel clamp this class applies at t<=1 is therefore
// already the suite's "never exceed the lit master" hard ceiling, with no
// separate per-pixel comparison against a shipped lit-reference image
// needed at runtime (see tests/gui/AdditiveGlowTests.cpp's round-trip test,
// which DOES load the lit reference - off the runtime binary-data path,
// purely as a one-time test fixture - to verify this construction claim
// once).
//
// juce::Graphics::setOpacity()/Colour::withAlpha(float) jassert if given a
// value outside [0,1] (JUCE 8.0.14 juce_Colour.cpp) - this class therefore
// NEVER passes t directly as an opacity when t may exceed 1.0 (the bezel's
// deliberate startup overshoot, see drawRing()'s docs); instead it
// precomputes a second "overshoot" frame at construction and cross-blends
// between the two precomputed frames, each blend stage using an opacity
// strictly within [0,1].
namespace basilica::gui
{
    class AdditiveGlow
    {
    public:
        AdditiveGlow() = default;

        // offPlateImage: the runtime baseline (master_alchemie.png).
        // glowImage: the design's own additive diff sprite for this zone
        // (glows.json's per-sprite RGB=colour/alpha=magnitude convention).
        // glowOffsetInMaster: where glowImage's own (0,0) sits within
        // offPlateImage's coordinate space (glows.json's offsetX/offsetY).
        // additiveGain: glows.json's own top-level "additiveGain" (1.0).
        // overshootGain: how far past 1.0 drawRing()'s t may reach before
        // being clamped - default 1.0 (no overshoot frame is built) for
        // zones that never overshoot. The bezel passes ~1.154 here (see
        // PluginEditorLayout.h's bezelOvershootPeakT, measured from the
        // startup easing curve's own analytic peak - see PluginEditor.cpp's
        // easeOutBackWithOvershoot() docs).
        AdditiveGlow (const juce::Image& offPlateImage, const juce::Image& glowImage,
                     juce::Point<int> glowOffsetInMaster, float additiveGain = 1.0f,
                     float overshootGain = 1.0f);

        bool isValid() const noexcept { return litImage.isValid(); }

        // Draws the FULL glow footprint (no angular restriction) at overall
        // intensity t, clamped to [0, overshootGain] (the value passed at
        // construction). t in [0,1] cross-blends the caller's own
        // already-drawn baseline toward the precomputed t=1 "lit" frame; t
        // in (1, overshootGain] cross-blends from the "lit" frame toward
        // the precomputed "overshoot" frame - see class docs for why this
        // two-stage precomputed-frame approach exists rather than a direct
        // opacity=t call. Used for the bezel's one-shot startup power-up.
        void drawRing (juce::Graphics& g, juce::Rectangle<float> destRectOnScreen, float t) const;

        // Draws the glow footprint CLIPPED to an angular pie wedge,
        // sweeping clockwise from startAngleDeg toward startAngleDeg +
        // (endAngleDeg-startAngleDeg)*proportion (0 = fully hidden, 1 = the
        // sprite's own full measured arc), always at a fixed intensity of
        // 1.0 (no breathing/overshoot - the knob rings are a deterministic,
        // continuously-updating value display, not a ballistic/idle-flicker
        // element - see the M3 GUI briefing's signature-behaviour #1).
        // destRectOnScreen must be this glow zone's own on-screen footprint
        // (same convention as drawRing()). centreInMaster/startAngleDeg/
        // endAngleDeg are in the SAME (off-plate/master) pixel space
        // glowOffsetInMaster was given in - glows.json's own
        // centreX/centreY/startAngleDeg/endAngleDeg fields per ring. The
        // sprite's own alpha channel (zero outside its measured ring band)
        // does the real radial masking; this method's clip path only needs
        // to bound the ANGULAR sector.
        void drawWedge (juce::Graphics& g, juce::Rectangle<float> destRectOnScreen,
                        juce::Point<float> centreInMaster, float startAngleDeg, float endAngleDeg,
                        float proportion) const;

    private:
        juce::Image litImage;       // offPlate crop + rgb*(alpha/255)*additiveGain*1.0, clamped (the t=1 frame)
        juce::Image overshootImage; // offPlate crop + rgb*(alpha/255)*additiveGain*overshootGain, clamped - only built if overshootGain > 1.0
        juce::Point<int> glowOffset;
        int glowWidth = 0;
        int glowHeight = 0;
        float overshootGainValue = 1.0f;
    };
}
