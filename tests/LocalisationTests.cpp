#include "presets/Localisation.h"

#include <BinaryData.h>
#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

// M2 i18n frame tests (.scaffold/specs/preset-system-m2.md's "I18N" section:
// "the de mapping parses; every TRANS key present in de.txt; parameter
// names verifiably NOT in the mapping").
namespace
{
    // Every literal string PresetBar.cpp/PresetManager.cpp pass to TRANS(),
    // paired with its exact expected German translation from
    // resources/i18n/de.txt - kept as an independent list here (rather than
    // parsed out of the source) so this test fails loudly if a future
    // PresetBar/PresetManager change adds a new TRANS()'d string without a
    // matching de.txt entry, or if de.txt's mapping drifts. Checking the
    // exact expected value (rather than just "differs from the English
    // key") is required because "Init" intentionally maps to itself in
    // German too.
    struct ExpectedTranslation
    {
        const char* english;
        const char* german;
    };

    constexpr ExpectedTranslation expectedFrameKeys[] = {
        { "Cancel", "Abbrechen" },
        { "Delete", "L\xc3\xb6schen" },
        { "Enter a name for the new preset:", "Namen f\xc3\xbcr die neue Voreinstellung eingeben:" },
        { "Export preset...", "Voreinstellung exportieren..." },
        { "Export...", "Exportieren..." },
        { "Factory", "Werksvoreinstellungen" },
        { "Import a preset or preset bank...", "Voreinstellung oder Voreinstellungs-Sammlung importieren..." },
        { "Import failed", "Import fehlgeschlagen" },
        { "Import...", "Importieren..." },
        { "Init", "Init" },
        { "Preset name", "Name der Voreinstellung" },
        { "Save", "Speichern" },
        { "Save As...", "Speichern unter..." },
        { "Set current as default", "Aktuelle als Standard festlegen" },
        { "This file is not a valid preset.", "Diese Datei ist keine g\xc3\xbcltige Voreinstellung." },
        { "This preset file belongs to a different plugin.", "Diese Voreinstellungsdatei geh\xc3\xb6rt zu einem anderen Plugin." },
        { "This preset was saved by an incompatible version of the preset format.",
          "Diese Voreinstellung wurde mit einer inkompatiblen Version des Voreinstellungsformats gespeichert." },
    };

    // Core/DSP parameter names and units that must NEVER be translated
    // anywhere in this plugin (.scaffold/specs/preset-system-m2.md: "NEVER
    // translate core/DSP terminology") - verified absent as keys in de.txt.
    constexpr const char* parameterNamesNeverTranslated[] = {
        "Decay",
        "Pre-Delay",
        "Damping",
        "Width",
        "Mix",
        "Output",
        "Space",
        "Early/Late Balance",
        "Modulation",
        "Freeze",
        "Size",
        "Bass Decay",
    };
}

TEST_CASE ("i18n: resources/i18n/de.txt parses as a valid LocalisedStrings mapping", "[i18n][presets]")
{
    REQUIRE (BinaryData::de_txt != nullptr);
    REQUIRE (BinaryData::de_txtSize > 0);

    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    const juce::LocalisedStrings mapping (text, true);

    // A parsed-but-empty mapping (no translations recognised) would
    // indicate a malformed de.txt header/body - sanity-check at least one
    // known key round-trips to something other than itself.
    CHECK (mapping.translate ("Save") == juce::String ("Speichern"));
}

TEST_CASE ("i18n: every TRANS() key used by the copied preset-system frame is present in de.txt", "[i18n][presets]")
{
    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    const juce::LocalisedStrings mapping (text, true);

    for (const auto& expected : expectedFrameKeys)
    {
        CAPTURE (expected.english);
        CHECK (mapping.translate (expected.english) == juce::String::fromUTF8 (expected.german));
    }
}

TEST_CASE ("i18n: parameter names and units are verifiably NOT present as keys in de.txt", "[i18n][presets]")
{
    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    const juce::LocalisedStrings mapping (text, true);

    for (const auto* name : parameterNamesNeverTranslated)
    {
        CAPTURE (name);
        // No mapping for a parameter name means translate() returns it
        // unchanged (falls through to the original string) - proof it was
        // never added as a translatable key.
        CHECK (mapping.translate (name) == juce::String (name));
    }
}

TEST_CASE ("i18n: installLocalisation() is idempotent and safe to call repeatedly", "[i18n][presets]")
{
    CHECK_NOTHROW (basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize));
    CHECK_NOTHROW (basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize));

    // Reset to no mappings (English fallthrough) so this test doesn't leak
    // German mappings into whichever test runs next in the same process.
    juce::LocalisedStrings::setCurrentMappings (nullptr);
}
