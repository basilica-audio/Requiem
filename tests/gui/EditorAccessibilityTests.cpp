#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/HubNeedle.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// a11y coverage for every wired M3 photoreal-GUI control. Deliberately calls
// createAccessibilityHandler() directly rather than getAccessibilityHandler()
// - the latter (JUCE 8.0.14 juce_Component.cpp) only returns a handler once
// the component has a live native window peer, which this headless,
// no-message-loop test binary never has.
namespace
{
    template <typename ComponentType>
    ComponentType* findChildByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
            if (auto* typed = dynamic_cast<ComponentType*> (parent.getChildComponent (i)))
                if (typed->getTitle() == title)
                    return typed;

        return nullptr;
    }

    std::unique_ptr<juce::AccessibilityHandler> createHandlerForTest (juce::Component& component)
    {
        return component.createAccessibilityHandler();
    }
}

TEST_CASE ("Every wired knob exposes an accessible title, value, and declared unit", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    // All 5 physical knobs (see docs/gui-mapping.md's mapping table).
    struct Expectation
    {
        const char* label;
        const char* unitSuffix;
    };

    const Expectation expectations[] = {
        { "Decay", "s" },
        { "Pre-Delay", "ms" },
        { "Mix", "%" },
        { "Damping", "Hz" },
        { "Size", "%" },
    };

    for (const auto& expectation : expectations)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, expectation.label);
        REQUIRE (knob != nullptr);
        CHECK (knob->getTitle() == expectation.label);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("knob \"" << expectation.label << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.isNotEmpty());
        CHECK (valueText.endsWith (expectation.unitSuffix));
    }
}

TEST_CASE ("Freeze's accessible name matches its visual label and exposes a checkable state", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    auto* toggle = findChildByTitle<juce::ToggleButton> (editor, "Freeze");
    REQUIRE (toggle != nullptr);
    CHECK (toggle->getTitle() == juce::String ("Freeze"));

    const auto handler = createHandlerForTest (*toggle);
    REQUIRE (handler != nullptr);

    // juce::ToggleButton's own constructor calls
    // setClickingTogglesState(true) (JUCE 8.0.14 juce_ToggleButton.cpp),
    // so juce::Button::isToggleable() is true and the base juce::Button
    // AccessibilityHandler correctly exposes checkable/checked state.
    CHECK (handler->getCurrentState().isCheckable());
}

TEST_CASE ("IR override button exposes an accessible title, a press action, and is keyboard-operable", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    auto* irButton = findChildByTitle<juce::TextButton> (editor, "Impulse response loader");
    REQUIRE (irButton != nullptr);
    CHECK (irButton->getTitle() == juce::String ("Impulse response loader"));

    // Keyboard-operable via JUCE's ordinary Button focus/key handling (no
    // custom keyPressed() override needed) - Button::Button() itself calls
    // setWantsKeyboardFocus(true) (JUCE 8.0.14 juce_Button.cpp), so
    // Enter/Space trigger onClick through the base class's own key handling.
    CHECK (irButton->getWantsKeyboardFocus());

    // The click handler (opens the "Load IR.../Use procedural IR" menu) is
    // wired - not invoked here, since triggering it would launch a real
    // async juce::PopupMenu with no native window/message loop available in
    // this headless, no-message-loop test binary (see this file's top-of-file
    // docs).
    CHECK (irButton->onClick != nullptr);

    const auto handler = createHandlerForTest (*irButton);
    REQUIRE (handler != nullptr);
    CHECK (handler->getRole() == juce::AccessibilityRole::button);
    CHECK (handler->getActions().contains (juce::AccessibilityActionType::press));

    // Momentary menu launcher, not a toggle - unlike Freeze above.
    CHECK_FALSE (handler->getCurrentState().isCheckable());
}

TEST_CASE ("The needle (output level meter) exposes a read-only accessible value inside the real editor", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    auto* needle = findChildByTitle<basilica::gui::HubNeedle> (editor, "Output level meter");
    REQUIRE (needle != nullptr);

    const auto handler = createHandlerForTest (*needle);
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());
    CHECK (valueInterface->getCurrentValueAsString().endsWith ("dB"));
}

TEST_CASE ("Scale button's accessible title reflects the current scale percentage, not a static string", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    auto* scaleButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID ("scaleButton"));
    REQUIRE (scaleButton != nullptr);

    CHECK (scaleButton->getTitle().contains ("100%"));

    REQUIRE (scaleButton->onClick);
    scaleButton->onClick();

    CHECK (scaleButton->getButtonText() == "150%");
    CHECK (scaleButton->getTitle().contains ("150%"));
    CHECK_FALSE (scaleButton->getTitle().contains ("100%"));
}

// Issue #5 (keyboard navigation): juce::Slider ships with
// setWantsKeyboardFocus(false) in JUCE 8.0.14 (juce_Slider.cpp:1461,
// Slider::init), so InvisibleKnob was silently unreachable by Tab and its
// keyPressed()/focus ring never fired - and even when focused, the base
// keyPressed (juce_Slider.cpp:1029) steps by the raw parameter interval
// (0.1% on Mix's 100% range) and ignores Shift entirely. These tests pin
// the fixed contract (setWantsKeyboardFocus(true) + KeyboardSteps.h).

TEST_CASE ("Every interactive control is keyboard-focusable", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    int knobsSeen = 0, buttonsSeen = 0;

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* child = editor.getChildComponent (i);

        if (auto* slider = dynamic_cast<juce::Slider*> (child))
        {
            ++knobsSeen;
            INFO ("knob \"" << slider->getTitle().toStdString() << "\"");
            CHECK (slider->getWantsKeyboardFocus());
        }
        else if (auto* button = dynamic_cast<juce::Button*> (child))
        {
            ++buttonsSeen;
            INFO ("button \"" << button->getTitle().toStdString() << "\"");
            CHECK (button->getWantsKeyboardFocus());
        }
    }

    // All 5 crystal knobs and 3 buttons (Freeze, IR loader, scale) must be
    // present AND focusable - a zero-match loop must not pass vacuously.
    CHECK (knobsSeen == 5);
    CHECK (buttonsSeen == 3);
}

TEST_CASE ("Arrow keys step knobs by a practical amount, Shift+Arrow steps finer", "[gui][a11y]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    RequiemAudioProcessorEditor editor (processor);

    // Mix: linear 0..100 %, 0.1 interval (ParameterLayout.cpp) - the
    // base-class step would be 0.1 over a 100-unit range (1000 presses).
    auto* knob = findChildByTitle<juce::Slider> (editor, "Mix");
    REQUIRE (knob != nullptr);

    knob->setValue (50.0, juce::sendNotificationSync);

    // Called through Component& for the same [class.access.virt] reason
    // documented on createHandlerForTest().
    juce::Component& knobAsComponent = *knob;

    // Plain Right = 1% of the 100-unit range = 1.0.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (knob->getValue() == Catch::Approx (51.0).margin (1.0e-4));

    // Shift+Right = 0.1% = 0.1 (the keyboard analog of Shift-drag).
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                                          juce::ModifierKeys::shiftModifier, 0)));
    CHECK (knob->getValue() == Catch::Approx (51.1).margin (1.0e-4));

    // Plain Left steps back down symmetrically.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (knob->getValue() == Catch::Approx (50.1).margin (1.0e-4));

    // PageDown = 10% = 10.0.
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::pageDownKey)));
    CHECK (knob->getValue() == Catch::Approx (40.1).margin (1.0e-4));

    // Home/End jump to the range extremes (WAI-ARIA slider pattern).
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
    CHECK (knob->getValue() == Catch::Approx (0.0).margin (1.0e-4));
    REQUIRE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::endKey)));
    CHECK (knob->getValue() == Catch::Approx (100.0).margin (1.0e-4));

    // Ctrl/Cmd-modified presses are host shortcuts - never consumed.
    CHECK_FALSE (knobAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                                              juce::ModifierKeys::ctrlModifier, 0)));
    CHECK (knob->getValue() == Catch::Approx (100.0).margin (1.0e-4));
}
