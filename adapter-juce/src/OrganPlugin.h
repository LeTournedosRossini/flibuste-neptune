#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "HostProbe.h"
#include "KnobModal.h"
#include "NeptuneKnob.h"
#include "ParamHost.h"
#include "PresetHost.h"
#include "NovaOrganEditor.h"
#include "PresetManager.h"
#include "CarteDriver.h"
#include "neptune/core/I18n.h"
#include "neptune/core/OrganPreset.h"
#include "neptune/core/OrganSynth.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Theme.h"
#include "neptune/core/Undo.h"

// « Orgamon » (nom provisoire) : l'orgue à roues phoniques de Neptune (moteur de setBfree) en plugin CLAP instrument indépendant.
class OrganProcessor : public juce::AudioProcessor,
                       public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                       public ParamHost,
                       public PresetHost,
                       private juce::AsyncUpdater
{
public:
    OrganProcessor();
    ~OrganProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Orgamon"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }
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

    neptune::core::organ::OrganDescription organDescription() const { return engine_.description(); }
    void uiEditOrgan (neptune::core::organ::OrganDescription, std::string name, std::string mergeKey = {});
    void uiPanic() { engine_.postPanic(); }
    void uiNoteOn (int channel, int key) { engine_.postNoteOn (channel, key); }
    void uiNoteOff (int channel, int key) { engine_.postNoteOff (channel, key); }
    neptune::core::UndoManager& undoManager() { return undo_; }
    // ---- presets (PresetHost) ----
    neptune::core::preset::Library& library() override { return *library_; }
    std::string currentPresetId() const override { return currentPresetId_; }
    const neptune::core::preset::Preset* currentPreset() const override { return currentPresetId_.empty() ? nullptr : library_->find (currentPresetId_); }
    std::uint32_t presetRevision() const override { return presetRevision_; }
    void touchPresets() override { ++presetRevision_; }
    // ---- Carte : l'orgue joue d'après la carte partagée (voir CarteDriver.h)
    CarteDriver& carte() { return carte_; }
    bool ccEnabled() const noexcept { return ccEnabled_.load(); }
    void uiSetCcEnabled (bool on) { ccEnabled_.store (on); }
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
    bool acceptsPreset (const neptune::core::preset::Preset& p) const override { return p.module.empty() || p.module == neptune::core::organ::moduleId; }
    std::vector<std::string> presetKeys() const override
    {
        std::vector<std::string> keys;
        for (const auto& k : neptune::core::organ::keyedParams()) keys.push_back (k.key);
        return keys;
    }

    neptune::core::organ::OrganSynthEngine& engine() { return engine_; }
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    void applySettings (const neptune::core::settings::GlobalSettings&);   // met à jour et enregistre les réglages globaux de la suite (thème, contraste, langue)
    // Niveau d'entrée MIDI d'un canal (0..15) : vélocité de la dernière note enfoncée, 0 quand aucune n'est tenue (pour les vu-mètres de contrôle des claviers)
    float midiLevel (int channel) const noexcept { return midiLevel_[channel & 15].load (std::memory_order_relaxed); }
    float outPeak (int channel) const noexcept { return outPeak_[channel & 1].load (std::memory_order_relaxed); }   // crête du dernier bloc, pour le vu-mètre

private:
    static BusesProperties makeBuses();
    void handleAsyncUpdate() override;
    void notifyHostOfParameterChanges (const std::vector<float>& before);
    std::vector<float> snapshotNormalised() const;

    neptune::core::organ::OrganSynthEngine engine_;
    HostProbe probe_;
    neptune::core::UndoManager undo_;
    neptune::core::settings::GlobalSettings settings_;
    neptune::core::uistate::Size modal_[2] {};
    std::unique_ptr<neptune::core::preset::Library> library_;
    std::string currentPresetId_;
    std::uint32_t presetRevision_ = 1;
    std::atomic<int> midiBank_ { 0 };
    std::atomic<float> outPeak_[2] { 0.0f, 0.0f };
    std::atomic<float> midiLevel_[16] {};
    int heldPerChannel_[16] {};   // fil audio seulement
    std::atomic<bool> ccEnabled_ { false }; // contrôleurs MIDI (hors banque et notes) : désactivés par défaut
    std::atomic<int> pendingBank_ { -1 }, pendingProgram_ { -1 };

    CarteDriver carte_ { 1, 3, "orgamon" };   // clavier sur le canal 1 (upper), pédale sur le canal 3

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrganProcessor)
};

// Éditeur du plugin Orgamon (fenêtre CLAP) : thème, contraste, langue ; reconstruit l'interface quand ils changent.
class NovaOrganPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit NovaOrganPluginEditor (OrganProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void buildContent();
    OrganProcessor& processor_;
    neptune::core::i18n::Translator tr_;
    std::unique_ptr<NovaOrganEditor> content_;
    juce::ComponentBoundsConstrainer constrainer_;
};
