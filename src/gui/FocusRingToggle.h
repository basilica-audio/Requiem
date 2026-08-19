#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-reusable focus-visible toggle (issue #5, WCAG 2.4.7 Focus Visible).
//
// The editor's toggles are deliberately INVISIBLE juce::ToggleButtons (all
// tick/text colour IDs transparentBlack - the visible lever state is the
// paint()-drawn ToggleZoneSwap crop over the baked master render), and
// juce::LookAndFeel_V4::drawToggleButton (JUCE 8.0.14) draws no focus
// indication of its own for toggles - so keyboard focus on a stock
// ToggleButton here would be completely invisible. This subclass draws the
// same minimal, self-contained ring MasterCropKnob::paint() uses (no
// LookAndFeel dependency, to stay portable to sibling plugins), on top of
// whatever the base class painted.
namespace basilica::gui
{
    class FocusRingToggle : public juce::ToggleButton
    {
    public:
        using juce::ToggleButton::ToggleButton;

        void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
        {
            juce::ToggleButton::paintButton (g, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

            if (hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.5f);
            }
        }
    };
}
