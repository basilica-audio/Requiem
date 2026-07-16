#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

namespace
{
    constexpr int knobSize = 90;
    constexpr int textBoxHeight = 20;
    constexpr int labelHeight = 20;
    constexpr int margin = 20;
    constexpr int numKnobs = 6;
    constexpr int editorWidth = margin * 2 + numKnobs * knobSize + (numKnobs - 1) * margin;

    constexpr int presetBarHeight = 28;

    // Second row: Space combo + Early/Late Balance knob + Modulation knob +
    // Freeze toggle.
    constexpr int secondRowHeight = knobSize;
    constexpr int spaceComboWidth = 140;
    constexpr int freezeButtonWidth = 90;

    // Third row (v0.2.0 additions): Size knob + Bass Decay knob.
    constexpr int thirdRowHeight = knobSize;

    constexpr int irRowHeight = 30;
    constexpr int editorHeight = margin * 6 + presetBarHeight + labelHeight + knobSize + textBoxHeight
                                  + secondRowHeight + thirdRowHeight + irRowHeight;

    // M2 i18n frame (.scaffold/specs/preset-system-m2.md): selects German
    // (resources/i18n/de.txt) or falls through to English, once, at editor
    // construction - see Localisation.h's docs. `presetBar` is a member
    // initialised via the constructor's initialiser list, and its own
    // constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order regardless of the order
    // they're written in, so this helper (called from presetBar's own
    // initialiser expression below) is what actually guarantees
    // installLocalisation() runs before presetBar exists, not an
    // installLocalisation() call in the constructor *body*, which would run
    // too late.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (RequiemAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

RequiemAudioProcessorEditor::RequiemAudioProcessorEditor (RequiemAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    addAndMakeVisible (presetBar);

    configureKnob (decayKnob, ParamIDs::decay, "Decay");
    configureKnob (preDelayKnob, ParamIDs::preDelay, "Pre-Delay");
    configureKnob (dampingKnob, ParamIDs::damping, "Damping");
    configureKnob (widthKnob, ParamIDs::width, "Width");
    configureKnob (mixKnob, ParamIDs::mix, "Mix");
    configureKnob (outputKnob, ParamIDs::output, "Output");

    spaceLabel.setText ("Space", juce::dontSendNotification);
    spaceLabel.setJustificationType (juce::Justification::centred);
    spaceLabel.attachToComponent (&spaceCombo, false);
    addAndMakeVisible (spaceLabel);

    // Populate the combo directly from the parameter's own choices, so the
    // GUI can never drift out of sync with ParameterLayout.cpp's ordering.
    if (auto* spaceParam = dynamic_cast<juce::AudioParameterChoice*> (audioProcessor.apvts.getParameter (ParamIDs::space)))
    {
        int itemId = 1;
        for (const auto& choice : spaceParam->choices)
            spaceCombo.addItem (choice, itemId++);
    }
    addAndMakeVisible (spaceCombo);
    spaceAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, ParamIDs::space, spaceCombo);

    configureKnob (earlyLateBalanceKnob, ParamIDs::earlyLateBalance, "Early/Late");
    configureKnob (modulationKnob, ParamIDs::modulation, "Modulation");

    configureKnob (sizeKnob, ParamIDs::size, "Size");
    configureKnob (bassDecayKnob, ParamIDs::bassDecay, "Bass Decay");

    addAndMakeVisible (freezeButton);
    freezeAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, ParamIDs::freeze, freezeButton);

    loadIrButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> ("Load impulse response...",
                                                             juce::File(),
                                                             "*.wav;*.aif;*.aiff");

        constexpr auto chooserFlags = juce::FileBrowserComponent::openMode
                                       | juce::FileBrowserComponent::canSelectFiles;

        fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();

            if (file != juce::File())
                audioProcessor.loadUserImpulseResponseFile (file);

            updateIrStatusLabel();
        });
    };
    addAndMakeVisible (loadIrButton);

    clearIrButton.onClick = [this]
    {
        audioProcessor.clearUserImpulseResponseFile();
        updateIrStatusLabel();
    };
    addAndMakeVisible (clearIrButton);

    irStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (irStatusLabel);
    updateIrStatusLabel();

    setResizable (false, false);
    setSize (editorWidth, editorHeight);
}

RequiemAudioProcessorEditor::~RequiemAudioProcessorEditor() = default;

void RequiemAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText)
{
    knob.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, knobSize, textBoxHeight);
    addAndMakeVisible (knob.slider);

    knob.label.setText (labelText, juce::dontSendNotification);
    knob.label.setJustificationType (juce::Justification::centred);
    // false => label sits above the slider it tracks; JUCE repositions it
    // automatically whenever the slider's bounds change, so resized() only
    // needs to place the sliders themselves.
    knob.label.attachToComponent (&knob.slider, false);
    addAndMakeVisible (knob.label);

    knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, knob.slider);
}

void RequiemAudioProcessorEditor::updateIrStatusLabel()
{
    if (audioProcessor.isUsingUserImpulseResponse())
        irStatusLabel.setText ("IR: " + audioProcessor.getUserImpulseResponseFile().getFileName(), juce::dontSendNotification);
    else
        irStatusLabel.setText ("IR: procedural (Decay/Damping/Space)", juce::dontSendNotification);
}

void RequiemAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (margin);

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));
    bounds.removeFromTop (margin);

    auto irRow = bounds.removeFromBottom (irRowHeight);
    loadIrButton.setBounds (irRow.removeFromLeft (100));
    irRow.removeFromLeft (margin / 2);
    clearIrButton.setBounds (irRow.removeFromLeft (80));
    irRow.removeFromLeft (margin / 2);
    irStatusLabel.setBounds (irRow);

    bounds.removeFromBottom (margin);

    // Third row (v0.2.0 additions): Size + Bass Decay.
    auto thirdRow = bounds.removeFromBottom (thirdRowHeight);
    thirdRow.removeFromTop (labelHeight); // room for the attached labels above these knobs

    sizeKnob.slider.setBounds (thirdRow.removeFromLeft (knobSize));
    thirdRow.removeFromLeft (margin);
    bassDecayKnob.slider.setBounds (thirdRow.removeFromLeft (knobSize));

    bounds.removeFromBottom (margin);

    // Second row: Space combo + Early/Late Balance + Modulation + Freeze.
    auto secondRow = bounds.removeFromBottom (secondRowHeight);
    secondRow.removeFromTop (labelHeight); // room for the Space label above the combo

    auto spaceArea = secondRow.removeFromLeft (spaceComboWidth);
    spaceCombo.setBounds (spaceArea.withSizeKeepingCentre (spaceComboWidth, textBoxHeight));
    secondRow.removeFromLeft (margin);

    earlyLateBalanceKnob.slider.setBounds (secondRow.removeFromLeft (knobSize));
    secondRow.removeFromLeft (margin);
    modulationKnob.slider.setBounds (secondRow.removeFromLeft (knobSize));
    secondRow.removeFromLeft (margin);
    freezeButton.setBounds (secondRow.removeFromLeft (freezeButtonWidth).withSizeKeepingCentre (freezeButtonWidth, textBoxHeight));

    bounds.removeFromBottom (margin);

    bounds.removeFromTop (labelHeight); // room for the attached labels above each first-row knob

    const auto slotWidth = bounds.getWidth() / numKnobs;

    for (auto* knob : { &decayKnob, &preDelayKnob, &dampingKnob, &widthKnob, &mixKnob, &outputKnob })
        knob->slider.setBounds (bounds.removeFromLeft (slotWidth).reduced (margin / 2, 0));
}
