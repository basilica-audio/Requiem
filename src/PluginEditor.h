#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>

#include "PluginEditorLayout.h"
#include "gui/AdditiveGlow.h"
#include "gui/HubNeedle.h"
#include "gui/FocusRingButton.h"
#include "gui/FocusRingToggle.h"
#include "gui/InvisibleKnob.h"
#include "gui/PlateTypography.h"
#include "presets/PresetBar.h"

class RequiemAudioProcessor;

// M3 photoreal GUI (the "alchemie" faceplate design) - reuses
// basilica-audio/aureate's M3 pilot architecture (a single baked master
// image + small live overlays, the preset-bar/scale-step editor frame, and
// HubNeedle) but adapts the interaction/lighting model where this design
// genuinely differs from tubecomp - see docs/gui-mapping.md for the full
// component-reuse-vs-adaptation table:
//   1. baseline master (paint()) - the aubergine panel, silver engravings,
//      moon-dial bezel (unlit), 5 black crystal knobs (unlit ring
//      channels), 2 buttons, 2 dark glass windows - all baked, single image.
//   2. 5x InvisibleKnob (own child components) - plain hit-areas; the
//      crystal knobs themselves NEVER repaint (no pointer on a faceted
//      crystal - see InvisibleKnob.h's docs).
//   3. 4x AdditiveGlow::drawWedge() (paint()) - the four outer knobs' ring
//      halos, angularly clipped to each knob's own live normalised value
//      (signature behaviour #1).
//   4. 1x AdditiveGlow::drawRing() (paint()) - the bezel's one-shot startup
//      power-up, eased with a brief overshoot (signature behaviour #2).
//   5. HubNeedle (own child component) - the moon-dial needle only; the
//      dial face itself stays fully baked.
//   6. Freeze button pressed-state: a minimal vector darken overlay
//      (paint()) - see docs/gui-mapping.md for why (no pressed-state crop
//      asset exists for this design revision, unlike tubecomp's
//      toggle-N-down.png family).
//   7. IR override button (buttonRight1x): opens a juce::PopupMenu
//      ("Load IR..." / "Use procedural IR") wired to the existing,
//      unmodified user-IR backend (RequiemAudioProcessor::
//      loadUserImpulseResponseFile()/clearUserImpulseResponseFile()/
//      isUsingUserImpulseResponse()). Reuses the Freeze button's own vector
//      overlay technique for both its transient pressed-state darken and,
//      while a user IR is active, a persistent subtle brightness LIFT
//      (paint()) - see docs/gui-mapping.md's button-mapping table.
class RequiemAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit RequiemAudioProcessorEditor (RequiemAudioProcessor& processorToEdit);
    ~RequiemAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Test/preview-only: headless test binaries have no running message
    // loop to pump real timer ticks through (see
    // tests/gui/EditorSnapshotTests.cpp's own docs). Normal operation never
    // calls these.
    void setBezelGlowElapsedSecondsForPreview (double elapsedSeconds) noexcept;
    void setBezelGlowSettledForPreview() noexcept;

private:
    void timerCallback() override;
    void updateBezelGlow() noexcept;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        std::unique_ptr<basilica::gui::InvisibleKnob> slider;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText);
    void applyScaleStep (int newStepIndex);
    void cycleScale();
    void drawPlateTypography (juce::Graphics& g, juce::Point<float> plateOrigin, float scale) const;
    void showIrMenu();
    void repaintButtonZone (const rqm::layout::ButtonSlot1x& slot) noexcept;

    RequiemAudioProcessor& audioProcessor;

    juce::Image masterImage;

    basilica::presets::PresetBar presetBar;
    juce::TextButton scaleButton;
    int scaleStepIndex = 0; // 0 = 100%, 1 = 150%, 2 = 200%

    std::unique_ptr<basilica::gui::HubNeedle> needle;

    static constexpr int numKnobs = 5;
    std::array<Knob, numKnobs> knobs;

    // 4 outer knobs' ring glows, parallel to rqm::layout::knobRingZones
    // (NOT to knobs[] directly - the centre knob at knobSlots1x[2] has no
    // ring entry, see centreKnobSlotIndex).
    basilica::gui::PlateTypography typography;
    std::array<basilica::gui::AdditiveGlow, 4> knobRingGlows;

    basilica::gui::AdditiveGlow bezelGlow;
    double bezelGlowStartTimeSeconds = 0.0;
    float bezelGlowT = 0.0f;

    // Freeze - the only true boolean parameter in Requiem's APVTS (see
    // docs/gui-mapping.md's mapping table). Bound to buttonLeft1x.
    // FocusRingToggle/FocusRingButton rather than the plain JUCE types
    // (issue #5, WCAG 2.4.7): both controls are intentionally invisible,
    // so keyboard focus needs its own visible ring - see the two headers.
    std::unique_ptr<basilica::gui::FocusRingToggle> freezeButton;
    std::unique_ptr<ButtonAttachment> freezeAttachment;

    // IR override entry point (buttonRight1x) - restores the pre-M3 editor's
    // "Load IR..."/"Clear IR" feature through a juce::PopupMenu ("Load IR..."
    // / "Use procedural IR") rather than dedicated buttons/label, since only
    // one physical button remains. No APVTS attachment: the user-IR override
    // is processor-owned file state, not a parameter (see PluginProcessor.h's
    // loadUserImpulseResponseFile()/clearUserImpulseResponseFile()/
    // isUsingUserImpulseResponse()).
    std::unique_ptr<basilica::gui::FocusRingButton> irButton;
    std::unique_ptr<juce::FileChooser> irFileChooser;

    // Cache of isUsingUserImpulseResponse(), polled once per timer tick
    // (timerCallback()) so the persistent "lit" marker overlay repaints
    // promptly if the active/procedural state changes for a reason other
    // than this button's own menu (e.g. a host reloading session state with
    // a previously-saved user IR while the editor is already open).
    bool irButtonLitCache = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RequiemAudioProcessorEditor)
};
