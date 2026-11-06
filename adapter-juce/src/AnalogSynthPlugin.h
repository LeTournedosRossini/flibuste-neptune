#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "CarteDriver.h"
#include "HostProbe.h"
#include "ModulationHost.h"
#include "ParamHost.h"
#include "NovaSynthEditor.h"
#include "PresetHost.h"
#include "PresetManager.h"
#include "neptune/core/AnalogSynth.h"
#include "neptune/core/I18n.h"
#include "neptune/core/ModulationRig.h"
#include "neptune/core/Preset.h"
#include "neptune/core/PresetLibrary.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « AnalogSynth » (nom provisoire) : le synthé soustractif de Neptune publié comme plugin CLAP instrument indépendant.
// Même moteur (neptune::core::analogsynth), interface NOVA (NovaSynthEditor), plus le système de presets (Preset.h) : banques 0..127 pilotées
// par MIDI Bank Select / Program Change, grille concert, bibliothèque utilisateur sur disque, presets livrés en lecture seule.
class AnalogSynthProcessor : public juce::AudioProcessor,
                             public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                             public ParamHost,
                             public PresetHost,
                             public ModulationHost,   // matrice de modulation du plugin (onglet MOD)
                             private juce::AsyncUpdater
{
public:
    AnalogSynthProcessor();
    ~AnalogSynthProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AnalogSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- ParamHost ----
    neptune::core::ParameterSet& parameters() override { return engine_.params(); }
    void uiBeginEdit (neptune::core::ParamId) override;
    void uiSetNormalised (neptune::core::ParamId, double) override;
    void uiEndEdit (neptune::core::ParamId) override;
    void uiResetToDefault (neptune::core::ParamId) override;
    bool uiUndo() override;
    bool uiRedo() override;
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0]; }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { modal_[t == neptune::core::uistate::Terminal::Touch ? 1 : 0] = s; }

    // ---- patch du synthé (description non automatisable) ----
    neptune::core::SynthDescription synthDescription() const { return engine_.description(); }
    void uiEditSynth (neptune::core::SynthDescription, std::string name, std::string mergeKey = {});
    void uiBeginGesture() { undo_.beginGesture(); }
    void uiEndGesture() { undo_.endGesture(); }
    neptune::core::UndoManager& undoManager() { return undo_; }

    // ---- notes de l'interface ----
    void uiNoteOn (int key, float velocity) { engine_.postNoteOn (key, velocity); }
    void uiNoteOff (int key) { engine_.postNoteOff (key); }
    void uiPanic() { engine_.postPanic(); }
    // molettes de l'écran (thread de message) : lues par le thread audio à chaque bloc, comme si elles venaient du MIDI
    void uiPitchBend (float position) noexcept { uiBend_.store (position); }
    void uiModWheel (float position) noexcept { uiMod_.store (position); }

    // ---- presets (thread de message) : PresetHost ----
    neptune::core::preset::Library& library() override { return *library_; }
    std::string currentPresetId() const override { return currentPresetId_; }
    const neptune::core::preset::Preset* currentPreset() const override { return currentPresetId_.empty() ? nullptr : library_->find (currentPresetId_); }
    std::uint32_t presetRevision() const override { return presetRevision_; }
    void touchPresets() override { ++presetRevision_; }
    bool loadPreset (const std::string& id) override;
    bool stepPreset (int direction) override;
    bool isModified() const override;
    bool canSaveCurrent() const override;
    bool saveCurrent (std::string* error = nullptr) override;
    bool overwritePreset (const std::string& id, std::string* error = nullptr) override;
    bool saveCurrentAs (const std::string& name, const std::string& type, neptune::core::preset::Preset* out = nullptr,
                        std::string* error = nullptr) override;
    bool saveConfig (const neptune::core::preset::LibraryConfig&) override;
    const neptune::core::preset::LibraryConfig& presetConfig() const override { return library_->config(); }
    bool acceptsPreset (const neptune::core::preset::Preset& p) const override
    {
        return p.module.empty() || p.module == neptune::core::analogsynth::moduleId;
    }
    std::vector<std::string> presetKeys() const override
    {
        std::vector<std::string> keys;
        for (const auto& k : neptune::core::analogsynth::keyedParams()) keys.push_back (k.key);
        keys.push_back (neptune::core::analogsynth::glideKey);
        return keys;
    }

    // ---- ModulationHost : la matrice du plugin (une seule « piste », la 0) ----
    int modTrack() const noexcept override { return 0; }
    neptune::core::modulation::Matrix modMatrix (int) const override { return rig_.matrix(); }
    void modEdit (int, neptune::core::modulation::Matrix) override;
    void modBeginGesture() override { undo_.beginGesture(); }
    void modEndGesture() override { undo_.endGesture(); }
    neptune::core::ParameterSet& modParameters() override { return engine_.params(); }
    std::string modDestName (int, neptune::core::ParamId dest) const override;
    bool modShowScope() const override { return false; }   // un seul module : pas de menu « Piste 1 / Globale »

    neptune::core::analogsynth::AnalogSynthEngine& engine() { return engine_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    CarteDriver& carte() { return carte_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)

private:
    static BusesProperties makeBuses();
    void handleAsyncUpdate() override; // Program Change MIDI : appliqué sur le thread de message
    std::string modulationText() const;
    void notifyHostOfParameterChanges (const std::vector<float>& before);
    std::vector<float> snapshotNormalised() const;

    neptune::core::analogsynth::AnalogSynthEngine engine_;
    neptune::core::modulation::Rig rig_;   // matrice, LFO, enveloppes et sources MIDI du plugin
    HostProbe probe_;
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};
    std::unique_ptr<neptune::core::preset::Library> library_;
    std::string currentPresetId_;
    std::uint32_t presetRevision_ = 1;

    CarteDriver carte_ { 1, 1, "analogsynth" };   // joue d'après la carte partagée : tout sur le canal 1 (un seul instrument, pédale comprise)
    std::atomic<float> uiBend_ { 0.0f }, uiMod_ { 0.0f };   // molettes de l'écran
    float lastUiBend_ = 0.0f, lastUiMod_ = 0.0f;              // thread audio : dernières positions transmises
    std::atomic<int> midiBank_ { 0 };           // dernier CC 0 reçu (Bank Select MSB)
    std::atomic<int> pendingBank_ { -1 }, pendingProgram_ { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalogSynthProcessor)
};

// Fenêtre du plugin AnalogSynth : thème, contraste, langue ; reconstruit l'interface quand ils changent.
class NovaSynthPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit NovaSynthPluginEditor (AnalogSynthProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void buildContent();
    AnalogSynthProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    std::unique_ptr<NovaSynthEditor> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
