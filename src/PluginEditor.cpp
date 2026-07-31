#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

#include <array>
#include <cmath>
#include <utility>

namespace
{
    // Base (@1x, 100% scale) faceplate geometry lives in PluginEditorLayout.h
    // (rqm::layout) rather than here, so tests/gui/EditorLayoutTests.cpp can
    // assert layout invariants against the exact constants this file lays
    // components out with.
    using namespace rqm::layout;

    struct KnobLayoutEntry
    {
        int slotIndex;          // index into rqm::layout::knobSlots1x
        const char* parameterId;
        const char* labelText;  // accessible name only - no baked text labels
    };

    // Mapping decided for this M3 GUI pass (docs/gui-mapping.md has the full
    // rationale table): the centre knob (slot 2, the larger octagonal
    // crystal) carries Mix, the single most load-bearing control for a
    // reverb's dry/wet blend. The four ring-lit outer knobs carry the next
    // tier of continuous controls, reading left-to-right as two "time"
    // controls flanking the mix (Decay, Pre-Delay) and two "space/tone"
    // controls on the right (Damping, Size) - a deliberate signal-flow-ish
    // grouping given only 5 physical knob positions exist in this design
    // (vs. aureate's 10). Every other Requiem parameter (Width, Output,
    // Space, Early/Late Balance, Modulation, Bass Decay, and the whole
    // v0.3.0 "Living Tail" set) stays host-automatable/preset-only, not
    // physically knobbed this revision - same trade-off aureate's own M3
    // pilot made reducing its 21-parameter set down to 10 physical knobs.
    constexpr std::array<KnobLayoutEntry, 5> knobLayout {{
        { 0, ParamIDs::decay, "Decay" },
        { 1, ParamIDs::preDelay, "Pre-Delay" },
        { 2, ParamIDs::mix, "Mix" },
        { 3, ParamIDs::damping, "Damping" },
        { 4, ParamIDs::size, "Size" },
    }};

    // Requiem's needle displays the processor's own post-engine output
    // level (RequiemAudioProcessor::getCurrentOutputLevelDb(), already
    // atomic/real-time-safe - see PluginProcessor.h's docs), mapped
    // directly across the dial's own measured sweep - see
    // src/gui/HubNeedle.cpp's `ticks` table for the exact two calibration
    // points (this function exists only so PluginEditor.cpp doesn't need
    // to know HubNeedle's internal representation is "degrees"; HubNeedle
    // itself does the dB->angle mapping).
    float needleDbFromOutputLevelDb (float outputLevelDb) noexcept
    {
        return juce::jlimit (needleLevelFloorDb, needleLevelCeilingDb, outputLevelDb);
    }

    juce::Image loadImage (const char* data, int size)
    {
        return juce::ImageCache::getFromMemory (data, size);
    }

    // M2 i18n frame: selects German (resources/i18n/de.txt) or falls through
    // to English, once, at editor construction - see Localisation.h's docs.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (RequiemAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }

    // Non-parameter, per-session UI state: the stepped scale choice (0/1/2)
    // stored as a plain property directly on apvts.state.
    constexpr const char* uiScaleStepProperty = "uiScaleStep";

    // Startup bezel-glow animation curve: a standard "ease-out-back" cubic
    // (f(0)=0, f(1)=1 exactly, rising above 1.0 partway through before
    // settling back down to exactly 1.0 at x=1) - closed-form, so no spring
    // simulation/state is needed. backC1=2.2 was tuned (see this file's own
    // development notes / tests/gui/EditorSnapshotTests.cpp's numeric
    // verification) to land the curve's own analytic peak at ~115.4%,
    // matching the owner's brief ("brief overshoot (~115%)") - the peak
    // occurs at x~=0.542, i.e. ~0.65s into the 1.2s animation window.
    constexpr float backC1 = 2.2f;
    constexpr float backC3 = backC1 + 1.0f;

    float easeOutBackWithOvershoot (float x) noexcept
    {
        const auto clamped = juce::jlimit (0.0f, 1.0f, x);
        const auto x1 = clamped - 1.0f;
        return 1.0f + backC3 * x1 * x1 * x1 + backC1 * x1 * x1;
    }
}

RequiemAudioProcessorEditor::RequiemAudioProcessorEditor (RequiemAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    masterImage = loadImage (BinaryData::master_alchemie_png, BinaryData::master_alchemie_pngSize);

    // Creation order doubles as the accessibility/keyboard focus order
    // (JUCE's default FocusTraverser walks children in z-order, i.e.
    // creation order) - kept matching visual reading order: preset bar +
    // scale control, the needle/moon-dial, the knob row left-to-right, then
    // Freeze.
    addAndMakeVisible (presetBar);

    scaleButton.setComponentID ("scaleButton");
    scaleButton.onClick = [this] { cycleScale(); };
    addAndMakeVisible (scaleButton);

    basilica::gui::HubNeedle::Assets needleAssets;
    needleAssets.needleSprite = loadImage (BinaryData::needle_alchemie_png, BinaryData::needle_alchemie_pngSize);
    needle = std::make_unique<basilica::gui::HubNeedle> (
        needleAssets, "Output level meter",
        needleSpritePivotFraction, needleSpritePivotFraction,
        needleSpriteSizeFraction, needleBakedAngleDeg);
    addAndMakeVisible (*needle);

    for (size_t i = 0; i < knobLayout.size(); ++i)
    {
        auto& entry = knobLayout[i];
        knobs[(size_t) entry.slotIndex].slider = std::make_unique<basilica::gui::InvisibleKnob>();
        configureKnob (knobs[(size_t) entry.slotIndex], entry.parameterId, entry.labelText);
    }

    const struct
    {
        const char* data;
        int size;
    } glowKnobAssets[4] = {
        { BinaryData::glow_knob_1_png, BinaryData::glow_knob_1_pngSize },
        { BinaryData::glow_knob_2_png, BinaryData::glow_knob_2_pngSize },
        { BinaryData::glow_knob_3_png, BinaryData::glow_knob_3_pngSize },
        { BinaryData::glow_knob_4_png, BinaryData::glow_knob_4_pngSize },
    };

    for (size_t i = 0; i < knobRingZones.size(); ++i)
    {
        const auto& zone = knobRingZones[i];
        const auto glowImage = loadImage (glowKnobAssets[i].data, glowKnobAssets[i].size);
        knobRingGlows[i] = basilica::gui::AdditiveGlow (
            masterImage, glowImage, { zone.zoneOffsetXMasterPx, zone.zoneOffsetYMasterPx }, 1.0f, 1.0f);

        // Signature behaviour #1: the ring updates immediately on every
        // drag/automation write, not on a poll - see class docs.
        if (auto* slider = knobs[(size_t) zone.knobSlotIndex].slider.get())
        {
            slider->onValueChange = [this, i]
            {
                const auto& z = knobRingZones[i];
                const auto scale = scaleSteps[(size_t) scaleStepIndex];
                const auto s = [scale] (int v) { return (int) std::lround ((float) v * scale); };
                const auto yOffset = s (topStripHeight1x + topStripGap1x);
                repaint (juce::Rectangle<int> (s (z.zoneX1x), yOffset + s (z.zoneY1x), s (z.zoneW1x), s (z.zoneH1x)).expanded (s (4)));
            };
        }
    }

    const auto bezelGlowImage = loadImage (BinaryData::glow_bezel_png, BinaryData::glow_bezel_pngSize);
    bezelGlow = basilica::gui::AdditiveGlow (
        masterImage, bezelGlowImage, { bezelGlowZoneMasterPx[0], bezelGlowZoneMasterPx[1] }, 1.0f, bezelOvershootPeakT);
    bezelGlowStartTimeSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0;

    freezeButton = std::make_unique<juce::ToggleButton> (juce::String());
    freezeButton->setColour (juce::ToggleButton::tickColourId, juce::Colours::transparentBlack);
    freezeButton->setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::transparentBlack);
    freezeButton->setColour (juce::ToggleButton::textColourId, juce::Colours::transparentBlack);
    freezeButton->setTitle ("Freeze");
    freezeButton->setName ("Freeze");
    // The pressed-state darken overlay (paint()) is driven by the button's
    // own toggle state, not a separate value copy - just repaint on change.
    freezeButton->onStateChange = [this] { repaintButtonZone (buttonLeft1x); };
    addAndMakeVisible (*freezeButton);
    freezeAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, ParamIDs::freeze, *freezeButton);

    // IR override entry point (buttonRight1x) - see PluginEditor.h's
    // top-of-file docs / docs/gui-mapping.md. A plain juce::TextButton
    // (transparent, no baked pressed-state crop exists for this design) so
    // it stays keyboard-operable (Enter/Space triggers onClick) via JUCE's
    // ordinary Button focus handling - no custom key handling needed.
    irButton = std::make_unique<juce::TextButton> (juce::String());
    irButton->setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    irButton->setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    irButton->setColour (juce::TextButton::textColourOffId, juce::Colours::transparentBlack);
    irButton->setColour (juce::TextButton::textColourOnId, juce::Colours::transparentBlack);
    irButton->setTitle ("Impulse response loader");
    irButton->setName ("Impulse response loader");
    // Same technique as freezeButton->onStateChange above: the pressed-state
    // darken overlay (paint()) is driven by the button's own isDown() state,
    // transient rather than persistent (this button is momentary, not a
    // toggle) - just repaint its own zone on any state change.
    irButton->onStateChange = [this] { repaintButtonZone (buttonRight1x); };
    irButton->onClick = [this] { showIrMenu(); };
    addAndMakeVisible (*irButton);
    irButtonLitCache = audioProcessor.isUsingUserImpulseResponse();

    setResizable (false, false);

    const auto storedStep = (int) audioProcessor.apvts.state.getProperty (uiScaleStepProperty, 0);
    applyScaleStep (juce::jlimit (0, (int) scaleSteps.size() - 1, storedStep));

    startTimerHz (30);
}

RequiemAudioProcessorEditor::~RequiemAudioProcessorEditor() = default;

// Repaints exactly the (scaled, offset-by-the-top-strip) screen rect a given
// @1x button slot occupies, expanded by a few px of safety margin. Shared by
// both buttons' vector overlays (Freeze's toggle darken, the IR button's
// pressed darken and lit marker) so every caller stays scale-step-correct
// without duplicating the plate-origin math.
void RequiemAudioProcessorEditor::repaintButtonZone (const rqm::layout::ButtonSlot1x& slot) noexcept
{
    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    const auto s = [scale] (int v) { return (int) std::lround ((float) v * scale); };
    const auto yOffset = s (topStripHeight1x + topStripGap1x);
    const auto diameter = s (slot.diameter1x);
    repaint (juce::Rectangle<int> (0, 0, diameter, diameter)
                 .withCentre ({ s (slot.cx1x), yOffset + s (slot.cy1x) })
                 .expanded (s (4)));
}

// IR override menu (buttonRight1x) - restores the pre-M3 editor's
// "Load IR..."/"Clear IR" feature (see PluginProcessor.h's docs) through a
// juce::PopupMenu ("Load IR..." / "Use procedural IR") rather than dedicated
// buttons, since only one physical button remains in this design. Async
// throughout: showMenuAsync() rather than the (JUCE_MODAL_LOOPS_PERMITTED-
// gated) blocking show(), and juce::FileChooser::launchAsync() rather than
// any blocking browse - verified against JUCE 8.0.14's
// juce_PopupMenu.h/juce_FileChooser.h; no modal loop runs inside a plugin
// editor here.
void RequiemAudioProcessorEditor::showIrMenu()
{
    enum MenuItemId
    {
        loadIrItemId = 1,
        useProceduralIrItemId = 2,
    };

    const auto userIrActive = audioProcessor.isUsingUserImpulseResponse();

    juce::PopupMenu menu;
    menu.addItem (loadIrItemId, "Load IR...");
    menu.addItem (juce::PopupMenu::Item ("Use procedural IR")
                      .setID (useProceduralIrItemId)
                      // Ticked = already the active state; only clickable
                      // (enabled) when a user IR is currently overriding it,
                      // i.e. there is actually something to clear.
                      .setTicked (! userIrActive)
                      .setEnabled (userIrActive));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (*irButton),
        [this] (int result)
        {
            if (result == loadIrItemId)
            {
                // Kept alive on `this` (irFileChooser) for the duration of
                // the async browse - a local unique_ptr would be destroyed
                // (cancelling the chooser) as soon as this lambda returns.
                irFileChooser = std::make_unique<juce::FileChooser> ("Load impulse response...",
                                                                      juce::File(),
                                                                      "*.wav;*.aif;*.aiff");

                constexpr auto chooserFlags = juce::FileBrowserComponent::openMode
                                               | juce::FileBrowserComponent::canSelectFiles;

                irFileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& chooser)
                {
                    const auto file = chooser.getResult();

                    // loadUserImpulseResponseFile() itself validates
                    // (readable audio, <=30s - see PluginProcessor.h's
                    // docs) and gracefully no-ops/returns false on a bogus
                    // file; nothing further to do here either way.
                    if (file != juce::File())
                        audioProcessor.loadUserImpulseResponseFile (file);

                    irButtonLitCache = audioProcessor.isUsingUserImpulseResponse();
                    repaintButtonZone (rqm::layout::buttonRight1x);
                });
            }
            else if (result == useProceduralIrItemId)
            {
                audioProcessor.clearUserImpulseResponseFile();
                irButtonLitCache = audioProcessor.isUsingUserImpulseResponse();
                repaintButtonZone (rqm::layout::buttonRight1x);
            }
            // result == 0 (dismissed without a choice): nothing to do.
        });
}

void RequiemAudioProcessorEditor::cycleScale()
{
    applyScaleStep ((scaleStepIndex + 1) % (int) scaleSteps.size());
}

void RequiemAudioProcessorEditor::applyScaleStep (int newStepIndex)
{
    scaleStepIndex = juce::jlimit (0, (int) scaleSteps.size() - 1, newStepIndex);
    audioProcessor.apvts.state.setProperty (uiScaleStepProperty, scaleStepIndex, nullptr);

    const auto percentText = juce::String ((int) (scaleSteps[(size_t) scaleStepIndex] * 100.0f)) + "%";
    scaleButton.setButtonText (percentText);
    scaleButton.setTitle ("Window scale, " + percentText);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];

    setSize ((int) std::lround ((float) baseEditorWidth * scale),
             (int) std::lround ((float) baseEditorHeight * scale));
}

void RequiemAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText)
{
    knob.slider->setPopupDisplayEnabled (true, true, this);
    knob.slider->setTitle (labelText);
    knob.slider->setName (labelText);
    addAndMakeVisible (*knob.slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        const auto defaultValue = param->getNormalisableRange().convertFrom0to1 (param->getDefaultValue());
        knob.slider->setDoubleClickReturnValue (true, defaultValue);
    }

    // SliderAttachment MUST be constructed before the textFromValueFunction
    // override below - JUCE 8.0.14's SliderParameterAttachment constructor
    // itself assigns slider.textFromValueFunction as part of wiring the
    // attachment, which would silently clobber an override set beforehand.
    knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, *knob.slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        knob.slider->textFromValueFunction = [param] (double v)
        {
            return param->getText (param->convertTo0to1 ((float) v), 0) + " " + param->getLabel();
        };
        knob.slider->updateText();
    }
}

void RequiemAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    const auto s = [scale] (float v) { return v * scale; };

    const auto stripHeight = (float) topStripHeight1x * scale;
    // Aubergine-toned strip, matching the alchemie panel's own palette
    // (rather than tubecomp's amber) - sampled from
    // brand/mocks/alchemie/master-03-glows-off.png's own panel colour.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff231c2e), 0.0f, 0.0f,
                                             juce::Colour (0xff0d0a10), 0.0f, stripHeight, false));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), stripHeight));
    g.setColour (juce::Colour (0xff6a5a8a));
    g.fillRect (juce::Rectangle<float> (0.0f, stripHeight - 1.0f * scale, (float) getWidth(), 1.0f * scale));

    const auto plateOrigin = juce::Point<float> (0.0f, stripHeight + (float) topStripGap1x * scale);
    const auto plateBounds = juce::Rectangle<float> (plateOrigin.x, plateOrigin.y,
                                                      (float) plateWidth1x * scale, (float) plateHeight1x * scale);

    const auto toScreenRect = [&] (int x1x, int y1x, int w1x, int h1x)
    {
        return juce::Rectangle<float> (plateOrigin.x + s ((float) x1x),
                                       plateOrigin.y + s ((float) y1x),
                                       s ((float) w1x),
                                       s ((float) h1x));
    };

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

    // 1. Baseline plate: the single master render, filling the plate bounds.
    // Bakes the aubergine panel, silver engravings, moon-dial bezel (unlit),
    // 5 knobs (unlit ring channels), 2 buttons, 2 dark glass windows.
    if (masterImage.isValid())
        g.drawImage (masterImage, plateBounds, juce::RectanglePlacement::centred, false);

    // (2. The 5 knobs are separate InvisibleKnob child components, drawn
    // automatically after this method returns - they paint nothing but an
    // occasional keyboard-focus ring, see InvisibleKnob.h.)

    // 3. Knob ring glows (signature behaviour #1, ADDITIVE - see
    // AdditiveGlow.h) - each of the 4 outer knobs' ring wedge, clipped to
    // its own live normalised parameter value.
    for (size_t i = 0; i < knobRingZones.size(); ++i)
    {
        const auto& zone = knobRingZones[i];
        const auto destRect = toScreenRect (zone.zoneX1x, zone.zoneY1x, zone.zoneW1x, zone.zoneH1x);

        if (auto* slider = knobs[(size_t) zone.knobSlotIndex].slider.get())
        {
            const auto proportion = (float) slider->valueToProportionOfLength (slider->getValue());

            // centreInMaster passed to drawWedge() must be in the SAME
            // (off-plate/master) pixel space the zone's own offset is -
            // AdditiveGlow does the master->screen scaling internally from
            // destRect vs. the zone's own construction-time footprint.
            knobRingGlows[i].drawWedge (g, destRect, { zone.centreXMasterPx, zone.centreYMasterPx },
                                        zone.startAngleDeg, zone.endAngleDeg, proportion);
        }
    }

    // 4. Bezel glow (signature behaviour #2, ADDITIVE) - the one-shot
    // startup power-up, driven by bezelGlowT (see updateBezelGlow()).
    {
        const auto destRect = toScreenRect (bezelGlowZone1x[0], bezelGlowZone1x[1], bezelGlowZone1x[2], bezelGlowZone1x[3]);
        bezelGlow.drawRing (g, destRect, bezelGlowT);
    }

    // 5. Freeze pressed-state: a minimal vector darken overlay - see
    // PluginEditor.h's top-of-file docs for why (no pressed-state crop
    // asset exists for this design revision).
    if (freezeButton != nullptr && freezeButton->getToggleState())
    {
        const auto diameter = s ((float) buttonLeft1x.diameter1x);
        const auto centre = juce::Point<float> (plateOrigin.x + s ((float) buttonLeft1x.cx1x),
                                                 plateOrigin.y + s ((float) buttonLeft1x.cy1x));
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillEllipse (juce::Rectangle<float> (diameter, diameter).withCentre (centre));
    }

    // 6. IR override lit marker: while a user-supplied IR is active, a
    // persistent subtle brightness LIFT on the right button - the same
    // vector-overlay technique as Freeze's darken above, just additive
    // (lighter) instead of subtractive, so the override reads as
    // "engaged" without any new hand-drawn ornament (derived from the same
    // master crop the button itself is baked into - see docs/gui-mapping.md).
    if (audioProcessor.isUsingUserImpulseResponse())
    {
        const auto diameter = s ((float) buttonRight1x.diameter1x);
        const auto centre = juce::Point<float> (plateOrigin.x + s ((float) buttonRight1x.cx1x),
                                                 plateOrigin.y + s ((float) buttonRight1x.cy1x));
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.fillEllipse (juce::Rectangle<float> (diameter, diameter).withCentre (centre));
    }

    // 7. IR button pressed-state darken overlay - same technique/alpha as
    // Freeze's (see step 5), but transient (isDown()) rather than
    // persistent, since this button is momentary (a menu launcher), not a
    // toggle. Drawn on top of the lit marker so a click while a user IR is
    // already active still shows visible press feedback.
    if (irButton != nullptr && irButton->isDown())
    {
        const auto diameter = s ((float) buttonRight1x.diameter1x);
        const auto centre = juce::Point<float> (plateOrigin.x + s ((float) buttonRight1x.cx1x),
                                                 plateOrigin.y + s ((float) buttonRight1x.cy1x));
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillEllipse (juce::Rectangle<float> (diameter, diameter).withCentre (centre));
    }

    // (The needle is a separate HubNeedle child component, drawn after this
    // method returns - see resized() for its bounds. Everything else -
    // silver engravings, the moon-dial face itself, the crystal knobs' own
    // baked facets, the 2 dark glass windows - stays BAKED in the master,
    // no draw calls for any of it.)
}

void RequiemAudioProcessorEditor::resized()
{
    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    const auto s = [scale] (int v) { return (int) std::lround ((float) v * scale); };

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop (s (topStripHeight1x));

    scaleButton.setBounds (topStrip.removeFromRight (s (scaleButtonWidth1x)).reduced (0, s (2)));
    presetBar.setBounds (topStrip.reduced (0, s (2)));

    // Everything below is expressed in plate-local coordinates (the base
    // @1x table in PluginEditorLayout.h), then offset by the top strip +
    // gap and scaled.
    const auto toPlatePoint = [&] (juce::Point<int> plateLocal)
    {
        return juce::Point<int> (s (plateLocal.x),
                                 s (topStripHeight1x + topStripGap1x) + s (plateLocal.y));
    };

    const auto meterSize = s (meterComponentSize1x);
    const auto meterTopLeftScreen = toPlatePoint (meterTopLeft1x);
    needle->setBounds (meterTopLeftScreen.x, meterTopLeftScreen.y, meterSize, meterSize);

    for (const auto& slot : knobSlots1x)
    {
        const auto index = (size_t) (&slot - knobSlots1x.data());
        const auto diameter = s (slot.diameter1x);

        knobs[index].slider->setBounds (juce::Rectangle<int> (diameter, diameter)
                                            .withCentre (toPlatePoint ({ slot.cx1x, knobRowY1x })));
    }

    {
        const auto diameter = s (buttonLeft1x.diameter1x);
        freezeButton->setBounds (juce::Rectangle<int> (diameter, diameter)
                                     .withCentre (toPlatePoint ({ buttonLeft1x.cx1x, buttonLeft1x.cy1x })));
    }

    {
        const auto diameter = s (buttonRight1x.diameter1x);
        irButton->setBounds (juce::Rectangle<int> (diameter, diameter)
                                  .withCentre (toPlatePoint ({ buttonRight1x.cx1x, buttonRight1x.cy1x })));
    }
}

void RequiemAudioProcessorEditor::updateBezelGlow() noexcept
{
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const auto elapsed = (float) (now - bezelGlowStartTimeSeconds);

    if (elapsed >= bezelStartupDurationSeconds)
    {
        bezelGlowT = 1.0f;
        return;
    }

    bezelGlowT = easeOutBackWithOvershoot (elapsed / bezelStartupDurationSeconds);
}

void RequiemAudioProcessorEditor::timerCallback()
{
    needle->setTargetDb (needleDbFromOutputLevelDb (audioProcessor.getCurrentOutputLevelDb()));
    needle->tick (1.0f / 30.0f);

    const auto wasSettled = juce::approximatelyEqual (bezelGlowT, 1.0f);
    updateBezelGlow();

    if (! (wasSettled && juce::approximatelyEqual (bezelGlowT, 1.0f)))
    {
        const auto scale = scaleSteps[(size_t) scaleStepIndex];
        const auto s = [scale] (int v) { return (int) std::lround ((float) v * scale); };
        const auto yOffset = s (topStripHeight1x + topStripGap1x);
        repaint (juce::Rectangle<int> (s (bezelGlowZone1x[0]), yOffset + s (bezelGlowZone1x[1]),
                                       s (bezelGlowZone1x[2]), s (bezelGlowZone1x[3]))
                     .expanded (s (4)));
    }

    // IR override lit marker: poll rather than push, since
    // isUsingUserImpulseResponse() is processor-owned file state (not an
    // APVTS parameter/attachment) that can also change from causes other
    // than this button's own menu - e.g. a host reloading session state
    // (setStateInformation()) while the editor is already open.
    const auto userIrActiveNow = audioProcessor.isUsingUserImpulseResponse();

    if (userIrActiveNow != irButtonLitCache)
    {
        irButtonLitCache = userIrActiveNow;
        repaintButtonZone (buttonRight1x);
    }
}

void RequiemAudioProcessorEditor::setBezelGlowElapsedSecondsForPreview (double elapsedSeconds) noexcept
{
    bezelGlowStartTimeSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0 - elapsedSeconds;
    updateBezelGlow();
    repaint();
}

void RequiemAudioProcessorEditor::setBezelGlowSettledForPreview() noexcept
{
    bezelGlowT = 1.0f;
    repaint();
}
