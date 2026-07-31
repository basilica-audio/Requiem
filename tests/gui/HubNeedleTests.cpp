#include "gui/HubNeedle.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// HubNeedle's ballistic integration and dB->angle mapping are pure, static
// functions precisely so they're testable without a running juce::Timer/
// message loop (see HubNeedle.h's docs).
TEST_CASE ("HubNeedle::stepBallistics step response", "[gui]")
{
    using basilica::gui::HubNeedle;

    SECTION ("non-positive dt or tau snaps straight to target (defensive floor, never divides by zero)")
    {
        CHECK (HubNeedle::stepBallistics (-20.0f, 0.0f, 0.0f, 0.25f) == Catch::Approx (0.0f));
        CHECK (HubNeedle::stepBallistics (-20.0f, 0.0f, 1.0f / 30.0f, 0.0f) == Catch::Approx (0.0f));
    }

    SECTION ("repeated stepping monotonically approaches target without overshoot")
    {
        constexpr float tau = HubNeedle::ballisticsTauSeconds;
        constexpr float dt = 1.0f / 30.0f;
        constexpr float target = -10.0f;

        auto smoothed = -60.0f;
        auto previous = smoothed;

        for (int i = 0; i < 300; ++i)
        {
            smoothed = HubNeedle::stepBallistics (smoothed, target, dt, tau);
            CHECK (smoothed >= previous);
            CHECK (smoothed <= target);
            previous = smoothed;
        }

        CHECK (smoothed == Catch::Approx (target).margin (0.01f));
    }
}

// Requiem's alchemie moon-dial has no printed numerals - unlike the
// tubecomp pilot's 9-point tick table, this is a simple 2-point linear
// range (see HubNeedle.cpp's `ticks` docs for the measurement methodology).
TEST_CASE ("HubNeedle::tickAngleDegreesForDb linearly interpolates the alchemie dial's own measured sweep", "[gui]")
{
    using basilica::gui::HubNeedle;

    CHECK (HubNeedle::tickAngleDegreesForDb (-60.0f) == Catch::Approx (-130.0f));
    CHECK (HubNeedle::tickAngleDegreesForDb (0.0f) == Catch::Approx (130.0f));
    CHECK (HubNeedle::tickAngleDegreesForDb (-30.0f) == Catch::Approx (0.0f)); // exact midpoint -> straight up

    SECTION ("quarter points interpolate linearly")
    {
        CHECK (HubNeedle::tickAngleDegreesForDb (-45.0f) == Catch::Approx (-65.0f));
        CHECK (HubNeedle::tickAngleDegreesForDb (-15.0f) == Catch::Approx (65.0f));
    }

    SECTION ("values beyond the table clamp to the nearest end, never extrapolate")
    {
        CHECK (HubNeedle::tickAngleDegreesForDb (-100.0f) == Catch::Approx (-130.0f));
        CHECK (HubNeedle::tickAngleDegreesForDb (12.0f) == Catch::Approx (130.0f));
    }

    SECTION ("angle increases monotonically across the whole range")
    {
        float previous = HubNeedle::tickAngleDegreesForDb (-60.0f);

        for (float db = -55.0f; db <= 0.0f; db += 5.0f)
        {
            const auto deg = HubNeedle::tickAngleDegreesForDb (db);
            CHECK (deg > previous);
            previous = deg;
        }
    }
}

TEST_CASE ("HubNeedle exposes a read-only, unit-suffixed accessible value", "[gui][a11y]")
{
    basilica::gui::HubNeedle::Assets assets; // deliberately default/invalid image - fine, this test never calls paint()
    basilica::gui::HubNeedle needle (assets, "Output level meter", 0.5f, 0.5f, 0.98f, 0.0f);

    // createAccessibilityHandler() directly (not getAccessibilityHandler()) -
    // the latter only returns non-null once the component has a live native
    // window peer, which this headless, no-message-loop test binary never
    // has.
    const auto handler = needle.createAccessibilityHandler();
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);

    CHECK (valueInterface->isReadOnly());

    const auto valueText = valueInterface->getCurrentValueAsString();
    INFO ("HubNeedle accessible value = \"" << valueText.toStdString() << "\"");
    CHECK (valueText.endsWith ("dB"));
}

TEST_CASE ("HubNeedle::setImmediateDbForPreview seeds both target and smoothed reading immediately", "[gui]")
{
    basilica::gui::HubNeedle::Assets assets;
    basilica::gui::HubNeedle needle (assets, "Output level meter", 0.5f, 0.5f, 0.98f, 0.0f);

    needle.setImmediateDbForPreview (-7.0f);

    const auto handler = needle.createAccessibilityHandler();
    REQUIRE (handler != nullptr);
    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);

    CHECK (valueInterface->getCurrentValueAsString().getFloatValue() == Catch::Approx (-7.0f));
}
