#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/ParameterIds.h"
#include "params/ParameterLayout.h"

#include <BinaryData.h>

#include <cmath>

namespace
{
    // Rate at which the message-thread timer polls Decay/Damping for
    // changes and regenerates the procedural impulse response if needed.
    // Fast enough that the tail audibly updates promptly after a knob move,
    // slow enough to avoid reloading (and re-normalising) a multi-second IR
    // dozens of times a second while a slider is being dragged.
    constexpr int impulseResponseTimerHz = 20;

    //==========================================================================
    // M2 preset system (.scaffold/specs/preset-system-m2.md) - ported
    // verbatim from basilica-audio/nave's pilot implementation, see
    // docs/preset-system-notes.md's replication recipe.
    basilica::presets::PresetManagerConfig makePresetManagerConfig()
    {
        // JucePlugin_CFBundleIdentifier expands to a raw (unquoted) token
        // sequence, not a string literal - JUCE_STRINGIFY() is the
        // documented way to turn it into one (see JUCE's own
        // juce_CoreMidi_mac.mm for the same pattern). This is always
        // "com.yvesvogl.requiem" here (BUNDLE_ID in CMakeLists.txt),
        // matching the "plugin" field baked into every presets/factory/
        // *.json file.
        basilica::presets::PresetManagerConfig config;
        config.pluginId = JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier);
        config.pluginName = JucePlugin_Name;
        config.manufacturerName = "Basilica Audio";
        // Presets saved before the suite adopted its trading name still live
        // under the old folder. PresetManager copies them across on first
        // construction - copies, never moves, so an older build still finds
        // its own. See docs/branding.md.
        config.legacyManufacturerName = "Yves Vogl";
        config.pluginVersion = JucePlugin_VersionString;
        // userPresetsDirectoryOverrideForTests intentionally left
        // default-constructed (empty) - production instances always use the
        // real platform-standard preset location (see PresetManager.h).
        return config;
    }

    // BinaryData symbol names are derived from the presets/factory/*.json
    // file names passed to juce_add_binary_data() in CMakeLists.txt (dots
    // become underscores) - this list must stay in sync with that SOURCES
    // list. Order here only affects factory-preset iteration order before
    // getAllPresets() re-sorts alphabetically, so it isn't otherwise
    // significant.
    std::vector<basilica::presets::FactoryPresetAsset> makeFactoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::cathedralWash_json, BinaryData::cathedralWash_jsonSize },
            { BinaryData::concertHall_json, BinaryData::concertHall_jsonSize },
            { BinaryData::chamberRoom_json, BinaryData::chamberRoom_jsonSize },
            { BinaryData::choirBloom_json, BinaryData::choirBloom_jsonSize },
            { BinaryData::tightRhythmicHall_json, BinaryData::tightRhythmicHall_jsonSize },
            { BinaryData::frozenDrone_json, BinaryData::frozenDrone_jsonSize },
            { BinaryData::darkSanctuary_json, BinaryData::darkSanctuary_jsonSize },
            { BinaryData::brightSlapChamber_json, BinaryData::brightSlapChamber_jsonSize },
            { BinaryData::fullWetSendHall_json, BinaryData::fullWetSendHall_jsonSize },
            { BinaryData::subtleAir_json, BinaryData::subtleAir_jsonSize },
            // v0.3.0 "Living Tail" showcase presets.
            { BinaryData::livingCathedral_json, BinaryData::livingCathedral_jsonSize },
            { BinaryData::breathingChamber_json, BinaryData::breathingChamber_jsonSize },
            { BinaryData::bloomingHall_json, BinaryData::bloomingHall_jsonSize },
            { BinaryData::vintageLushPlate_json, BinaryData::vintageLushPlate_jsonSize },
            { BinaryData::dialogueDuckedScore_json, BinaryData::dialogueDuckedScore_jsonSize },
            { BinaryData::infiniteFrozenNave_json, BinaryData::infiniteFrozenNave_jsonSize },
        };
    }
}

//==============================================================================
RequiemAudioProcessor::RequiemAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      presetManager (apvts, makePresetManagerConfig(), makeFactoryPresetAssets())
{
    decaySeconds = apvts.getRawParameterValue (ParamIDs::decay);
    preDelayMs = apvts.getRawParameterValue (ParamIDs::preDelay);
    dampingHz = apvts.getRawParameterValue (ParamIDs::damping);
    widthPercent = apvts.getRawParameterValue (ParamIDs::width);
    mixPercent = apvts.getRawParameterValue (ParamIDs::mix);
    outputDb = apvts.getRawParameterValue (ParamIDs::output);
    spaceChoice = apvts.getRawParameterValue (ParamIDs::space);
    earlyLateBalancePercent = apvts.getRawParameterValue (ParamIDs::earlyLateBalance);
    modulationPercent = apvts.getRawParameterValue (ParamIDs::modulation);
    freezeToggle = apvts.getRawParameterValue (ParamIDs::freeze);
    sizePercent = apvts.getRawParameterValue (ParamIDs::size);
    bassDecayPercent = apvts.getRawParameterValue (ParamIDs::bassDecay);
    engineModeChoice = apvts.getRawParameterValue (ParamIDs::engineMode);
    tailModModeChoice = apvts.getRawParameterValue (ParamIDs::tailModMode);
    tailModDepthPercent = apvts.getRawParameterValue (ParamIDs::tailModDepth);
    tailModRatePercent = apvts.getRawParameterValue (ParamIDs::tailModRate);
    bloomAmountPercent = apvts.getRawParameterValue (ParamIDs::bloomAmount);
    lowCutHz = apvts.getRawParameterValue (ParamIDs::lowCut);
    highCutHz = apvts.getRawParameterValue (ParamIDs::highCut);
    duckAmountPercent = apvts.getRawParameterValue (ParamIDs::duckAmount);
    duckAttackMs = apvts.getRawParameterValue (ParamIDs::duckAttack);
    duckReleaseMs = apvts.getRawParameterValue (ParamIDs::duckRelease);

    jassert (decaySeconds != nullptr);
    jassert (preDelayMs != nullptr);
    jassert (dampingHz != nullptr);
    jassert (widthPercent != nullptr);
    jassert (mixPercent != nullptr);
    jassert (outputDb != nullptr);
    jassert (spaceChoice != nullptr);
    jassert (earlyLateBalancePercent != nullptr);
    jassert (modulationPercent != nullptr);
    jassert (freezeToggle != nullptr);
    jassert (sizePercent != nullptr);
    jassert (bassDecayPercent != nullptr);
    jassert (engineModeChoice != nullptr);
    jassert (tailModModeChoice != nullptr);
    jassert (tailModDepthPercent != nullptr);
    jassert (tailModRatePercent != nullptr);
    jassert (bloomAmountPercent != nullptr);
    jassert (lowCutHz != nullptr);
    jassert (highCutHz != nullptr);
    jassert (duckAmountPercent != nullptr);
    jassert (duckAttackMs != nullptr);
    jassert (duckReleaseMs != nullptr);

    // Resolves the default-resolution order (user "Default" preset >
    // factory "Default" preset > the ParameterLayout defaults already set
    // up above by APVTS's own constructor) - see PresetManager::
    // applyStartupDefault()'s docs.
    presetManager.applyStartupDefault();
}

RequiemAudioProcessor::~RequiemAudioProcessor()
{
    stopTimer();
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout RequiemAudioProcessor::createParameterLayout()
{
    return rqm::createParameterLayout();
}

//==============================================================================
const juce::String RequiemAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool RequiemAudioProcessor::acceptsMidi() const
{
    return false;
}

bool RequiemAudioProcessor::producesMidi() const
{
    return false;
}

bool RequiemAudioProcessor::isMidiEffect() const
{
    return false;
}

double RequiemAudioProcessor::getTailLengthSeconds() const
{
    // Matches the longest possible procedural IR (see ReverbIR::
    // maxDecaySeconds); a user-loaded IR could in principle be longer, but
    // there is no way to know its length until it's actually loaded, so
    // this is a reasonable upper-bound default for host tail-handling
    // (e.g. "stop processing silent tails after N seconds" logic).
    return static_cast<double> (ReverbIR::maxDecaySeconds);
}

int RequiemAudioProcessor::getNumPrograms()
{
    return 1;
}

int RequiemAudioProcessor::getCurrentProgram()
{
    return 0;
}

void RequiemAudioProcessor::setCurrentProgram (int)
{
}

const juce::String RequiemAudioProcessor::getProgramName (int)
{
    return {};
}

void RequiemAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void RequiemAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    // Seed the engine's parameters from the current APVTS state before
    // prepare() (re)generates the impulse response, so the very first block
    // after prepareToPlay() already reflects the host/session's actual
    // parameter values rather than the engine's built-in defaults.
    pushParametersToEngine();

    engine.prepare (spec);

    // juce::dsp::Convolution's default (zero-latency, uniformly
    // partitioned) configuration is used here, so this is normally 0 - see
    // ReverbEngine::getLatencySamples().
    setLatencySamples (engine.getLatencySamples());

    currentOutputLevelDb.store (-100.0f, std::memory_order_relaxed);

    startTimerHz (impulseResponseTimerHz);
}

void RequiemAudioProcessor::releaseResources()
{
    stopTimer();
}

void RequiemAudioProcessor::reset()
{
    engine.reset();
}

bool RequiemAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();

    if (mainOut != mono && mainOut != stereo)
        return false;

    if (mainOut != mainIn)
        return false;

    return true;
}

void RequiemAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Buses are constrained to in == out (mono or stereo), so this is
    // normally a no-op, but it's cheap insurance against stray channels.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    // Decay/Damping are only ever recorded here (an atomic store inside the
    // engine, no allocation) - the actual impulse-response regeneration
    // happens on the message thread via timerCallback(), never here.
    pushParametersToEngine();

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    // M3 GUI metering: instantaneous post-engine output magnitude (plain
    // atomic store, no allocation) - see getCurrentOutputLevelDb()'s docs.
    const auto numSamples = buffer.getNumSamples();

    if (numSamples > 0)
        currentOutputLevelDb.store (juce::Decibels::gainToDecibels (buffer.getMagnitude (0, numSamples), -100.0f),
                                    std::memory_order_relaxed);
}

//==============================================================================
void RequiemAudioProcessor::pushParametersToEngine() noexcept
{
    engine.setDecaySeconds (decaySeconds->load (std::memory_order_relaxed));
    engine.setDampingHz (dampingHz->load (std::memory_order_relaxed));
    engine.setPreDelayMs (preDelayMs->load (std::memory_order_relaxed));
    engine.setWidthPercent (widthPercent->load (std::memory_order_relaxed));
    engine.setMixProportion (mixPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputDb (outputDb->load (std::memory_order_relaxed));
    // AudioParameterChoice exposes its raw APVTS value as the choice index
    // (0/1/2 here), which maps 1:1 onto ReverbIR::SpaceType's declaration
    // order (see ParameterLayout.cpp).
    engine.setSpaceType (static_cast<ReverbIR::SpaceType> (juce::jlimit (0, 2,
        static_cast<int> (std::lround (spaceChoice->load (std::memory_order_relaxed))))));
    engine.setEarlyLateBalance (earlyLateBalancePercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setModulationAmount (modulationPercent->load (std::memory_order_relaxed) * 0.01f);
    // AudioParameterBool exposes its raw APVTS value as 0.0f/1.0f.
    engine.setFreeze (freezeToggle->load (std::memory_order_relaxed) >= 0.5f);
    engine.setSize (sizePercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setBassDecayMultiplier (bassDecayPercent->load (std::memory_order_relaxed) * 0.01f);

    // v0.3.0. Choice indices map 1:1 onto ReverbEngine::EngineMode and
    // FdnTail::ModulationMode's declaration order (see ParameterLayout.cpp).
    engine.setEngineMode (static_cast<ReverbEngine::EngineMode> (juce::jlimit (0, 2,
        static_cast<int> (std::lround (engineModeChoice->load (std::memory_order_relaxed))))));
    engine.setTailModMode (static_cast<FdnTail::ModulationMode> (juce::jlimit (0, 2,
        static_cast<int> (std::lround (tailModModeChoice->load (std::memory_order_relaxed))))));
    engine.setTailModDepth (tailModDepthPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setTailModRateScale (tailModRatePercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setBloomAmount (bloomAmountPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setLowCutHz (lowCutHz->load (std::memory_order_relaxed));
    engine.setHighCutHz (highCutHz->load (std::memory_order_relaxed));
    engine.setDuckAmountPercent (duckAmountPercent->load (std::memory_order_relaxed));
    engine.setDuckAttackMs (duckAttackMs->load (std::memory_order_relaxed));
    engine.setDuckReleaseMs (duckReleaseMs->load (std::memory_order_relaxed));
}

//==============================================================================
void RequiemAudioProcessor::timerCallback()
{
    // Message thread only (juce::Timer callbacks always run on the message
    // thread) - safe to do the allocation/generation/file-IO work that
    // regenerateImpulseResponseIfNeeded() may perform.
    engine.regenerateImpulseResponseIfNeeded();
}

//==============================================================================
bool RequiemAudioProcessor::loadUserImpulseResponseFile (const juce::File& file)
{
    return engine.loadUserImpulseResponse (file);
}

void RequiemAudioProcessor::clearUserImpulseResponseFile()
{
    engine.clearUserImpulseResponse();
}

//==============================================================================
bool RequiemAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* RequiemAudioProcessor::createEditor()
{
    return new RequiemAudioProcessorEditor (*this);
}

//==============================================================================
void RequiemAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());

    // The user IR file path isn't an APVTS parameter (it's a string, not
    // automatable), so it's persisted as a plain XML attribute alongside
    // the parameter state rather than through the ParameterLayout.
    xml->setAttribute (StateKeys::userIrPath,
                        engine.isUsingUserImpulseResponse()
                            ? engine.getUserImpulseResponseFile().getFullPathName()
                            : juce::String());

    // State schema version (new in v0.3.0). v0.1.x/v0.2.0 states carry no such
    // attribute at all; those are defined retroactively as schema 2 and are
    // recognised by its absence - see setStateInformation() below.
    xml->setAttribute (StateKeys::stateSchema, StateKeys::currentStateSchema);

    copyXmlToBinary (*xml, destData);
}

void RequiemAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    // Schema handling. A missing attribute (or "1"/"2") is a pre-v0.3.0 state:
    // APVTS's own tolerant load fills every parameter the XML does not mention
    // with its ParameterLayout default, and all ten v0.3.0 parameters default
    // to neutral values - Classic engine, hard-bypassed wet chain - so such a
    // session renders exactly as it did before. A *higher* schema than this
    // build knows about is loaded just as tolerantly: keep whatever parses,
    // ignore the rest, which is the same forward-compatibility stance v0.2.0
    // took for unknown parameter IDs.
    const auto loadedSchema = xmlState->getIntAttribute (StateKeys::stateSchema, 2);
    juce::ignoreUnused (loadedSchema);

    apvts.replaceState (juce::ValueTree::fromXml (*xmlState));

    const auto irPath = xmlState->getStringAttribute (StateKeys::userIrPath);

    if (irPath.isNotEmpty())
    {
        const juce::File irFile (irPath);

        // Best-effort restore: if the file has moved/been deleted since the
        // state was saved, silently fall back to the procedural generator
        // (driven by whatever Decay/Damping were just restored above)
        // rather than failing the whole state load.
        if (! engine.loadUserImpulseResponse (irFile))
            engine.clearUserImpulseResponse();
    }
    else
    {
        engine.clearUserImpulseResponse();
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RequiemAudioProcessor();
}
