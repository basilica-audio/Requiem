#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Requiem's own "alchemie" faceplate design knob interaction - NEW for this
// M3 GUI (not copied from basilica-audio/aureate's MasterCropKnob.h).
// alchemie's five crystal knobs are octagonal-faceted gemstones baked into
// the master render at a single fixed pose - there is no pointer/notch on a
// faceted crystal for a rotating-crop technique to convincingly animate
// (see the M3 GUI briefing: "The crystal knobs themselves do NOT rotate
// (no pointer on faceted crystal) - the ring IS the value display"), so
// this design instead uses a plain invisible juce::Slider hit-area over
// each knob's own baked art - the pattern the briefing calls "Aureate's
// passive pattern before MasterCropKnob" (i.e. before that pilot added its
// own rotating-crop technique). The knob's crystal itself never repaints;
// only the AdditiveGlow ring drawn separately in PluginEditor::paint()
// communicates the live value (see docs/gui-mapping.md).
//
// Kept as its own small reusable type (rather than configuring a bare
// juce::Slider inline) purely to carry MasterCropKnob's own fine-drag
// (Shift = 8x finer) UX convention forward without re-deriving it, and to
// give every knob a single, minimal, self-contained keyboard-focus
// indicator (WCAG 2.4.7) consistent with the rest of the suite's knobs.
namespace basilica::gui
{
    class InvisibleKnob : public juce::Slider
    {
    public:
        InvisibleKnob()
            : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox)
        {
            setMouseDragSensitivity (normalDragSensitivity);
            setScrollWheelEnabled (true);
            setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        }

        // Deliberately empty: the crystal knob's own baked art (drawn once,
        // as part of the static master render) never changes - this
        // component exists purely as a hit-area + accessibility/attachment
        // host. The one visible thing this override still draws is a
        // minimal keyboard-focus ring, mirroring MasterCropKnob::paint()'s
        // own WCAG 2.4.7 affordance.
        void paint (juce::Graphics& g) override
        {
            if (hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawEllipse (getLocalBounds().toFloat().reduced (1.0f), 1.5f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            setMouseDragSensitivity (e.mods.isShiftDown() ? fineDragSensitivity : normalDragSensitivity);
            Slider::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            setMouseDragSensitivity (e.mods.isShiftDown() ? fineDragSensitivity : normalDragSensitivity);
            Slider::mouseDrag (e);
        }

    private:
        static constexpr int normalDragSensitivity = 200;
        static constexpr int fineDragSensitivity = normalDragSensitivity * 8;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InvisibleKnob)
    };
}
