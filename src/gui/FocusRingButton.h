#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-reusable focus-visible text button (issue #5, WCAG 2.4.7 Focus
// Visible) - the TextButton sibling of FocusRingToggle.h.
//
// The IR-loader button is a deliberately INVISIBLE juce::TextButton (all
// four button/text colour IDs transparentBlack - the visible art is the
// baked master render underneath), and juce::LookAndFeel_V4::
// drawButtonBackground (JUCE 8.0.14) indicates keyboard focus purely by
// boosting the background colour's saturation - a no-op on a fully
// transparent background - so keyboard focus here would be completely
// invisible. This subclass draws the same minimal, self-contained ring
// InvisibleKnob::paint() uses (no LookAndFeel dependency, to stay portable
// to sibling plugins), on top of whatever the base class painted.
namespace basilica::gui
{
    class FocusRingButton : public juce::TextButton
    {
    public:
        using juce::TextButton::TextButton;

        void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            juce::TextButton::paintButton (g, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

            if (hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.5f);
            }
        }
    };
}
