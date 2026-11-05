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
#include "neptune/core/Arpeggiator.h"
#include "neptune/core/ChordFollower.h"
#include "neptune/core/ChordMapEnvironment.h"
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
    ~OrganProcessor() override { ticker_.stopTimer(); cancelPendingUpdate(); }

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
    // ---- Carte : l'orgue tient l'accord de la carte partagée pendant la lecture (voir docs/plan-harmonie-arpegiateur.md)
    bool carteFollow() const noexcept { return carteFollow_.load(); }
    void setCarteFollow (bool on) { carteFollow_.store (on); }
    int carteChannel() const noexcept { return carteChannel_.load(); }                 // canal de liaison, 1..8
    void setCarteChannel (int c) { carteChannel_.store (juce::jlimit (1, 8, c)); tick(); }
    int chordMidiChannel() const noexcept { return chordChannel_.load(); }             // canal MIDI des accords, 1..16
    void setChordMidiChannel (int c) { chordChannel_.store (juce::jlimit (1, 16, c)); }
    int bassMidiChannel() const noexcept { return bassChannel_.load(); }               // canal MIDI de la basse, 0 : pas de basse, 1..16
    void setBassMidiChannel (int c) { bassChannel_.store (juce::jlimit (0, 16, c)); }
    int carteApproach() const noexcept { return carteApproach_.load(); }              // 0 non, 1 dominante, 2 triton, 3 chromatique, 4 parallèle (motifs joués)
    void setCarteApproach (int k) { carteApproach_.store (juce::jlimit (0, 4, k)); }
    bool carteLed() const noexcept { return carteLed_.load(); }                        // voix conduites (défaut) ou position fixe
    void setCarteLed (bool on) { carteLed_.store (on); }
    int carteOctave() const noexcept { return carteOctave_.load(); }                   // octave de base, 1..6 (3 = do 48)
    void setCarteOctave (int o) { carteOctave_.store (juce::jlimit (1, 6, o)); }
    // Motifs : 16 emplacements, copiés dans l'état du projet (le projet reste autonome) ; le motif joué (0 = aucun : l'orgue tient l'accord) remplace le suivi d'accord.
    static constexpr int kMotifSlots = 16;
    int motifSlot() const noexcept { return motifSlot_.load(); }
    void setMotifSlot (int s) { motifSlot_.store (juce::jlimit (0, kMotifSlots, s)); }
    bool motifUsed (int index) const;
    juce::String motifName (int index) const;
    void setMotif (int index, const neptune::core::pattern::Pattern* p);   // nullptr : vide l'emplacement
    juce::String carteText() const;                                                    // « Carte : ON · canal 1 · Dm7 »
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

    // Carte : lecteur de la carte partagée (relevé depuis le fil de messages, consulté sans verrou par le fil audio)
    struct Ticker : juce::Timer
    {
        explicit Ticker (OrganProcessor& p) : owner (p) {}
        void timerCallback() override { owner.tick(); }
        OrganProcessor& owner;
    };
    void tick();
    neptune::core::chordmap::Reader reader_;
    neptune::core::arp::ChordMapEnvironment::Slots noSlots_;
    std::unique_ptr<neptune::core::arp::ChordMapEnvironment> carteEnv_;
    neptune::core::arp::ChordFollower follower_;
    std::array<neptune::core::arp::FollowEvent, 64> followOut_ {};
    Ticker ticker_ { *this };
    std::atomic<bool> carteFollow_ { false }, carteLed_ { true };
    std::atomic<int> carteApproach_ { 0 };
    std::atomic<int> carteChannel_ { 1 }, chordChannel_ { 1 }, bassChannel_ { 3 }, carteOctave_ { 3 };
    // Motifs (fil de messages : motifs_ sous verrou ; fil audio : copie audioMotifs_ refaite quand la version change)
    mutable juce::SpinLock motifLock_;
    neptune::core::arp::ChordMapEnvironment::Slots motifs_, audioMotifs_;
    std::vector<std::shared_ptr<const neptune::core::pattern::Pattern>> retiredMotifs_;
    std::atomic<int> motifsVersion_ { 0 }, motifSlot_ { 0 };
    int audioMotifsVersion_ = -1;
    neptune::core::arp::Arpeggiator arp_;
    std::array<neptune::core::arp::NoteOut, 256> arpOut_ {};
    std::atomic<int> carteChordNow_ { -2 };   // -2 : pas en lecture ou pas de Carte ; sinon racine | rel << 8

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
