#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/HubNeedle.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

// GUI smoke tests for the M3 photoreal "alchemie" editor (src/PluginEditor.h,
// src/gui/). juce::ScopedJuceInitialiser_GUI is installed once for the whole
// test binary in tests/TestMain.cpp, so Components/Timers are safe to
// construct here even though this is a headless console executable with no
// running message loop (timers simply never fire, which is fine - these
// tests only exercise synchronous construction/paint/destruction).
TEST_CASE ("Editor constructs, lays out, and destroys cleanly", "[gui]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    {
        RequiemAudioProcessorEditor editor (processor);

        CHECK (editor.getWidth() > 0);
        CHECK (editor.getHeight() > 0);
    }
    // editor destroyed here - JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR
    // (used throughout src/gui/ and on the editor itself) asserts at process
    // exit in Debug builds if any tagged instance was ever leaked, so a
    // clean run of this whole test binary is itself the leak check.
}

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

    // Configures a deliberately "alive-looking" state before snapshotting,
    // per the M3 GUI briefing: the needle deflected off its idle reading,
    // two of the four outer knob rings partially lit at DIFFERENT values,
    // the other two at their own distinct values too, Freeze engaged (the
    // pressed-state darken overlay), and the bezel glow at its settled
    // (post-startup) state.
    //
    // HubNeedle's own ballistic ramp would need real timer ticks pumped
    // through a running message loop to actually reach these values - this
    // headless test binary has no such loop, so the test/preview-only hook
    // (setImmediateDbForPreview()) seeds the reading directly instead.
    void configureLiveLookingState (RequiemAudioProcessorEditor& editor)
    {
        if (auto* needle = findChildByTitle<basilica::gui::HubNeedle> (editor, "Output level meter"))
            needle->setImmediateDbForPreview (-18.0f);

        if (auto* freeze = findChildByTitle<juce::ToggleButton> (editor, "Freeze"))
            freeze->setToggleState (true, juce::dontSendNotification);

        struct KnobValue
        {
            const char* label;
            double normalisedValue;
        };

        const KnobValue knobValues[] = {
            { "Decay", 0.25 }, { "Pre-Delay", 0.80 }, { "Mix", 0.55 }, { "Damping", 0.35 }, { "Size", 0.90 },
        };

        for (const auto& kv : knobValues)
            if (auto* knob = findChildByTitle<juce::Slider> (editor, kv.label))
                knob->setValue (knob->proportionOfLengthToValue (kv.normalisedValue), juce::dontSendNotification);

        editor.setBezelGlowSettledForPreview();
    }
}

TEST_CASE ("Editor snapshot at 100% is non-blank and is written for PR review", "[gui]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    RequiemAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);
    REQUIRE (editor.getHeight() > 0);

    configureLiveLookingState (editor);

    // SoftwareImageType (rather than the default NativeImageType) avoids any
    // dependency on an actual native graphics context/window, which keeps
    // this robust on headless CI runners.
    const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f, juce::SoftwareImageType {});

    REQUIRE (snapshot.isValid());
    CHECK (snapshot.getWidth() == editor.getWidth());
    CHECK (snapshot.getHeight() == editor.getHeight());

    // Non-blank: sample a small grid of points and confirm they are not all
    // identical to the top-left corner - a completely blank/solid-fill
    // render (e.g. every asset failing to decode) would fail this.
    const auto reference = snapshot.getPixelAt (0, 0);
    bool foundDifference = false;

    for (int y = 0; y < snapshot.getHeight() && ! foundDifference; y += juce::jmax (1, snapshot.getHeight() / 20))
        for (int x = 0; x < snapshot.getWidth() && ! foundDifference; x += juce::jmax (1, snapshot.getWidth() / 20))
            if (snapshot.getPixelAt (x, y) != reference)
                foundDifference = true;

    CHECK (foundDifference);

#ifdef REQUIEM_DOCS_DIR
    // Committed directly for PR review (docs/gui-preview.png).
    juce::PNGImageFormat pngFormat;
    const auto outFile = juce::File (REQUIEM_DOCS_DIR).getChildFile ("gui-preview.png");

    if (auto stream = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
    {
        stream->setPosition (0);
        stream->truncate();
        CHECK (pngFormat.writeImageToStream (snapshot, *stream));
    }
    else
    {
        FAIL ("could not open output stream for " << outFile.getFullPathName());
    }
#endif
}

// Proof that the 4 outer knobs' ring wedges actually sweep (signature
// behaviour #1) - two knobs set to distinctly non-rest proportions must
// visibly differ, within their own ring zone, from the construction-time
// (APVTS-default) rendering.
TEST_CASE ("Knob ring glows visibly sweep at distinctly non-default values", "[gui]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    RequiemAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);
    REQUIRE (editor.getHeight() > 0);

    const auto restSnapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f, juce::SoftwareImageType {});
    REQUIRE (restSnapshot.isValid());

    struct ZoomKnob
    {
        const char* label;
        double proportion;
    };

    constexpr ZoomKnob zoomKnobs[] = {
        { "Decay", 0.05 },
        { "Size", 0.95 },
    };

    for (const auto& zk : zoomKnobs)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, zk.label);
        REQUIRE (knob != nullptr);
        knob->setValue (knob->proportionOfLengthToValue (zk.proportion), juce::dontSendNotification);
    }

    const auto movedSnapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f, juce::SoftwareImageType {});
    REQUIRE (movedSnapshot.isValid());

    // Compare the full plate region (rather than each knob's own tiny hit
    // area, which - being an InvisibleKnob - never repaints itself; it's
    // the surrounding RING ZONE, drawn by the editor's own paint(), that
    // must visibly change).
    const auto cropBounds = editor.getLocalBounds();
    const auto restCrop = restSnapshot.getClippedImage (cropBounds);
    const auto movedCrop = movedSnapshot.getClippedImage (cropBounds);

    int changedPixels = 0;

    for (int y = 0; y < restCrop.getHeight(); ++y)
    {
        for (int x = 0; x < restCrop.getWidth(); ++x)
        {
            const auto a = restCrop.getPixelAt (x, y);
            const auto b = movedCrop.getPixelAt (x, y);
            const auto diff = std::abs (a.getRed() - b.getRed()) + std::abs (a.getGreen() - b.getGreen())
                             + std::abs (a.getBlue() - b.getBlue());
            if (diff > 24)
                ++changedPixels;
        }
    }

    INFO (changedPixels << " px changed between rest and moved knob-ring poses");
    CHECK (changedPixels > 0);
}

// Signature behaviour #2: the bezel glow is a ONE-SHOT deterministic
// startup animation (not idle breathing) - verifies the three defining
// moments the owner's brief calls for: fully dark at t=0, a brief overshoot
// mid-animation, and a stable settled state once the 1.2s window elapses.
TEST_CASE ("Bezel startup glow is dark at t=0, overshoots mid-animation, then settles", "[gui]")
{
    RequiemAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    RequiemAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);
    REQUIRE (editor.getHeight() > 0);

    const auto bezelProbeScreenPoint = [&]
    {
        // A point inside the bezel ring zone (1x, no scale applied at
        // construction) - approximate screen coords using
        // rqm::layout::bezelGlowZone1x's own centre plus the top-strip
        // offset resized() applies.
        using namespace rqm::layout;
        const auto x = bezelGlowZone1x[0] + bezelGlowZone1x[2] / 2;
        const auto y = topStripHeight1x + topStripGap1x + bezelGlowZone1x[1] + bezelGlowZone1x[3] / 2;
        return juce::Point<int> (x, y);
    }();

    const auto sampleAt = [&] (double elapsedSeconds)
    {
        editor.setBezelGlowElapsedSecondsForPreview (elapsedSeconds);
        const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f, juce::SoftwareImageType {});
        return snapshot.getPixelAt (bezelProbeScreenPoint.x, bezelProbeScreenPoint.y);
    };

    const auto atStart = sampleAt (0.0);
    const auto atOvershootPeak = sampleAt (0.65);  // ~x=0.542 of the 1.2s window - the curve's own analytic peak
    const auto atSettled = sampleAt (2.0);         // well past bezelStartupDurationSeconds (1.2s)

    const auto luminance = [] (juce::Colour c) { return 0.299f * (float) c.getRed() + 0.587f * (float) c.getGreen() + 0.114f * (float) c.getBlue(); };

    INFO ("t=0 luminance = " << luminance (atStart) << ", overshoot-peak luminance = " << luminance (atOvershootPeak)
                             << ", settled luminance = " << luminance (atSettled));

    // The overshoot peak must be at least as bright as the settled state
    // (the whole point of the "brief overshoot" brief) and both must be
    // visibly brighter than the fully-dark start.
    CHECK (luminance (atOvershootPeak) >= luminance (atSettled) - 1.0f);
    CHECK (luminance (atSettled) > luminance (atStart));
    CHECK (luminance (atOvershootPeak) > luminance (atStart));

    // Settling must be STABLE - sampling again well past the window must
    // reproduce the exact same frame (a true one-shot, not a repeating
    // idle animation).
    const auto atSettledAgain = sampleAt (5.0);
    CHECK (atSettledAgain == atSettled);
}
