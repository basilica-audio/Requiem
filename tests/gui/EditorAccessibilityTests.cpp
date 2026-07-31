#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/HubNeedle.h"

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
