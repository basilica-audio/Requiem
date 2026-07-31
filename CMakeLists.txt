cmake_minimum_required(VERSION 3.24)

# Must be set before the first project() call to take effect.
# CI overrides CMAKE_OSX_ARCHITECTURES explicitly for release/universal builds;
# local builds are left at the native architecture.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)

# JUCE's build tooling (juceaide) and some CMake find-modules it triggers
# (e.g. FindThreads) expect the C toolchain to be available; declaring it
# here at the top level avoids it being enabled implicitly deeper inside
# JUCE's CMake helpers, which has been observed to break Ninja's generate
# step (missing CMAKE_C_COMPILE_OBJECT rule variable).
project(Requiem VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/CPM.cmake)

# CPMAddPackage automatically respects the CPM_SOURCE_CACHE environment
# variable if it is set, reusing a shared package cache across builds/checkouts.
if(DEFINED ENV{CPM_SOURCE_CACHE})
    message(STATUS "Requiem: using CPM_SOURCE_CACHE at $ENV{CPM_SOURCE_CACHE}")
endif()

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE
)

# ==============================================================================
# Plugin target
# ==============================================================================

juce_add_plugin(Requiem
    COMPANY_NAME "Yves Vogl"
    BUNDLE_ID com.yvesvogl.requiem
    PLUGIN_MANUFACTURER_CODE Yvsv
    PLUGIN_CODE Rqem
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Requiem"
    ICON_BIG "docs/assets/icon.png"
    COPY_PLUGIN_AFTER_BUILD TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
)

# ==============================================================================
# Embedded assets: M2 preset system factory presets (presets/factory/*.json)
# and the M2 i18n frame's German translation (resources/i18n/de.txt). See
# src/presets/PresetManager.h and src/presets/Localisation.h - both consume
# these purely via BinaryData:: symbols passed in from PluginProcessor.cpp/
# PluginEditor.cpp, never by including BinaryData.h themselves, so they stay
# portable to sibling plugins (see docs/preset-system-notes.md in nave).
# ==============================================================================

juce_add_binary_data(RequiemBinaryData SOURCES
    presets/factory/default.json
    presets/factory/cathedralWash.json
    presets/factory/concertHall.json
    presets/factory/chamberRoom.json
    presets/factory/choirBloom.json
    presets/factory/tightRhythmicHall.json
    presets/factory/frozenDrone.json
    presets/factory/darkSanctuary.json
    presets/factory/brightSlapChamber.json
    presets/factory/fullWetSendHall.json
    presets/factory/subtleAir.json
    presets/factory/livingCathedral.json
    presets/factory/breathingChamber.json
    presets/factory/bloomingHall.json
    presets/factory/vintageLushPlate.json
    presets/factory/dialogueDuckedScore.json
    presets/factory/infiniteFrozenNave.json
    resources/i18n/de.txt
    # M3 photoreal GUI ("alchemie" faceplate design) - copied verbatim from
    # brand/mocks/alchemie/ (repo-relative to the suite root, one level
    # above this repo; see docs/gui-mapping.md for the full provenance and
    # parameter-mapping table). One baked UNLIT master faceplate plus its
    # small set of live-overlay source assets:
    #   - master_alchemie.png: the SOLE faceplate baseline (copied from
    #     master-03-glows-off.png) - aubergine panel, silver engravings,
    #     moon-dial bezel (unlit), 5 crystal knobs (unlit ring channels),
    #     2 buttons, 2 dark glass windows.
    #   - needle_alchemie.png: the master-extracted, pivot-centred moon-dial
    #     needle sprite (components/needle.png) - rotated live by
    #     src/gui/HubNeedle.
    #   - glow_knob_{1..4}.png / glow_bezel.png: the design's ADDITIVE light
    #     diff sprites (components/glow-knob-N.png / glow-bezel.png) -
    #     src/gui/AdditiveGlow composites these onto master_alchemie.png at
    #     runtime (see that class's own compositing-model docs). Note the
    #     LIT reference master (master-01-base.png) is intentionally NOT
    #     embedded here - it is only needed by the round-trip test in
    #     tests/gui/AdditiveGlowTests.cpp, which loads
    #     tests/data/alchemie-master-01-base-lit-reference.png directly
    #     from disk (see REQUIEM_TEST_DATA_DIR below), never via BinaryData.
    resources/gui/master_alchemie.png
    resources/gui/needle_alchemie.png
    resources/gui/glow_knob_1.png
    resources/gui/glow_knob_2.png
    resources/gui/glow_knob_3.png
    resources/gui/glow_knob_4.png
    resources/gui/glow_bezel.png
)

# ==============================================================================
# SharedCode: an INTERFACE target carrying the plugin's sources, definitions,
# and library links so that both the plugin targets (AU/VST3/Standalone) and
# the Tests executable compile the exact same code (Pamplejuce pattern, see
# https://github.com/sudara/pamplejuce), avoiding ODR violations between them.
# ==============================================================================

add_library(SharedCode INTERFACE)

target_compile_features(SharedCode INTERFACE cxx_std_20)

file(GLOB_RECURSE RequiemSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

target_sources(SharedCode INTERFACE ${RequiemSources})

target_include_directories(SharedCode INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(SharedCode INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # AGPLv3 usage permits disabling the splash screen.
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# Add any extra juce:: modules a specific plugin needs (e.g. juce_audio_formats
# for reading impulse-response files) to this list.
target_link_libraries(SharedCode INTERFACE
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    RequiemBinaryData
)

target_link_libraries(Requiem PRIVATE SharedCode)

# ==============================================================================
# Tests
# ==============================================================================

enable_testing()
set(CTEST_OUTPUT_ON_FAILURE ON)

CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.15.2
)

file(GLOB_RECURSE RequiemTestSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")

add_executable(Tests ${RequiemTestSources})

target_compile_features(Tests PRIVATE cxx_std_20)

target_link_libraries(Tests PRIVATE SharedCode Catch2::Catch2)

# Copy over the plugin target's compile definitions (JucePlugin_* macros etc.)
# so plugin code compiles identically when built as part of the Tests binary.
target_compile_definitions(Tests PRIVATE
    $<TARGET_PROPERTY:Requiem,COMPILE_DEFINITIONS>
    JUCE_MODAL_LOOPS_PERMITTED=1
    # Absolute path to tests/data, so tests/gui/AdditiveGlowTests.cpp can
    # load the LIT reference master (alchemie-master-01-base-lit-
    # reference.png) as a one-time round-trip test fixture, off the runtime
    # BinaryData path (see CMakeLists.txt's RequiemBinaryData docs above).
    REQUIEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    # Absolute path to docs/, so tests/gui/EditorSnapshotTests.cpp can write
    # the committed docs/gui-preview.png directly rather than a copy step.
    REQUIEM_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs"
)

set_target_properties(Tests PROPERTIES XCODE_GENERATE_SCHEME ON)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(RequiemDiscoveryMode PRE_TEST)
else()
    set(RequiemDiscoveryMode POST_BUILD)
endif()

catch_discover_tests(Tests DISCOVERY_MODE ${RequiemDiscoveryMode})
