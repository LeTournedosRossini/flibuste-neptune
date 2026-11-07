#pragma once
#include <unordered_map>
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <array>
#include <vector>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include "HostProbe.h"
#include "ModulationHost.h"
#include "ParamHost.h"
#include "PresetHost.h"
#include "neptune/core/OrganImport.h"
#include "neptune/core/SfzImport.h"
#include "neptune/core/SoundFont.h"
#include "neptune/core/AnalogSynth.h"
#include "neptune/core/FmPreset.h"
#include "neptune/core/AudioRecorder.h"
#include "neptune/core/Autosave.h"
#include "neptune/core/Control.h"
#include "OscServer.h"
#include "neptune/core/Osc.h"
#include "neptune/core/OrganSynth.h"
#include "neptune/core/Engine.h"
#include "neptune/core/Instrument.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/TrackExport.h"
#include "neptune/core/MixerState.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/Profile.h"
#include "neptune/core/Settings.h"
#include "neptune/core/Synth.h"
#include "neptune/core/Undo.h"
#include "neptune/core/UiState.h"

// Fine couche d'adaptation : traduit JUCE/CLAP <-> cœur. Aucune logique métier ici.
class NeptuneProcessor : public juce::AudioProcessor,
                         public clap_juce_extensions::clap_juce_audio_processor_capabilities,
                         public ParamHost,
                         public PresetHost, // presets du synthé de la piste sélectionnée (mêmes presets qu'AnalogSynth)
                         public ModulationHost // matrice de modulation de la piste sélectionnée (onglet MOD)
{
public:
    // Profil effectif -> moteur, plan de compensation, latence annoncée à l'hôte (thread de message ; aussi à l'activation et au chargement d'un état).
    void refreshLatency();
    NeptuneProcessor();
    ~NeptuneProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Neptune"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }   // messages MIDI envoyés à la main vers l'hôte (postMidi) ; le tampon MIDI est vidé après usage : aucun écho
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // État = JSON versionné produit/lu par le cœur (neptune/core/State.h).
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    neptune::core::ParameterSet& parameters() override { return params_; }
    int probeTracks() const { return probeTracks_; }
    HostProbe& hostProbe() { return hostProbe_; }

    // Réglages globaux (fichier du dossier de configuration utilisateur) et récupération après crash.
    const neptune::core::settings::GlobalSettings& settings() const { return settings_; }
    juce::String settingsFilePath() const { return juce::String::fromUTF8 (settingsFile_.u8string().c_str()); }
    bool hasRecoverableState() const;
    juce::String recoverableDescription() const; // date de l'autosave proposé
    void restoreRecovered();                     // thread message
    void discardRecovered();
    int probeOutputs() const { return probeOutputs_; }

    // Profil Live/Studio : réglage partagé du processus + verrou propre à l'instance (sauvegardé dans le projet).
    neptune::core::InstanceProfile& profile() { return profile_; }
    void notifyStateChanged(); // thread message : le verrou fait partie de l'état du projet

    // Édition depuis l'interface (thread message). Chaque geste prévient l'hôte (automation) ET alimente l'annulation.
    void uiBeginEdit (neptune::core::ParamId) override;
    void uiSetNormalised (neptune::core::ParamId, double normalised) override;
    void uiEndEdit (neptune::core::ParamId) override;
    void uiResetToDefault (neptune::core::ParamId) override;
    bool canAssignMacro (neptune::core::ParamId) const override;
    std::vector<int> macrosOf (neptune::core::ParamId) const override;
    void uiToggleMacro (neptune::core::ParamId, int macro) override;
    void uiClearMacros (neptune::core::ParamId) override;
    // Réserve d'automation : en liste réduite, les macros sont les emplacements que voit l'hôte ; un réglage s'y relie par clic droit (ReserveMenu.h).
    int reserveCount() const override { return settings_.reducedHostList ? neptune::core::control::kMacros : 0; }
    bool reserveSupportsLock() const override { return false; }
    bool reserveCanAssign (neptune::core::ParamId) const override;
    bool reserveDirect (neptune::core::ParamId) const override;
    int reserveSlotOf (neptune::core::ParamId) const override;
    std::string reserveSlotName (int slot) const override;
    int uiReserveAssign (neptune::core::ParamId, int slot, bool replace) override;
    void uiReserveRelease (neptune::core::ParamId id) override { uiClearMacros (id); }
    void uiReserveReleaseSlot (int slot) override;
    // Réglages des effets posés en insert : une macro peut les piloter (clic droit sur le potard d'un insert). Voir Control.h (Target::isInsert).
    std::string describeTarget (const neptune::core::control::Target&) const;                       // « Piste 1 - insert 2 Compresseur - Threshold »
    std::shared_ptr<neptune::core::InsertInstance> insertInstanceFor (const neptune::core::control::Target&) const;   // nul : emplacement vide ou d'un autre type
    neptune::core::control::TargetResolver controlResolver();
    static neptune::core::control::Target insertTarget (neptune::core::StripKind, int index, int slot, const std::string& type, neptune::core::ParamId param);
    int insertParamMacro (const neptune::core::control::Target&) const;                                // macro qui pilote ce réglage d'insert, ou -1
    int assignInsertParam (const neptune::core::control::Target&, int macro, bool replace);          // renvoie la macro utilisée, ou -1
    void releaseInsertParam (const neptune::core::control::Target&);
    juce::AudioProcessorParameter* hostParamForInsert (const neptune::core::control::Target&) const;   // macro de l'hôte qui porte seule ce réglage (gestes d'écriture d'automation)
    void reconcileInsertBindings();   // thread de message : retire les assignations dont l'insert a disparu ou changé de type, relie les autres au module actuel
    void refreshMacroNames();   // le nom de chaque macro vu par l'hôte (« 3 · Volume piste 2 ») et la table réglage -> macro des gestes d'édition
    std::string uiText (std::string_view key) const override;
    int mixerZoomPercent() const { return settings_.mixerZoomPercent; }
    void uiSetMixerZoomPercent (int percent);   // mémorisé dans les réglages globaux
    bool reducedHostList() const { return settings_.reducedHostList; }   // lu à la création de l'instance
    bool showDiagnostics() const { return settings_.showDiagnostics; }
    void uiSetLook (const std::string& theme, int contrast, const std::string& language);   // thème, contraste, langue : écrit settings.json ; l'éditeur se reconstruit avec la nouvelle palette
    void uiSetShowDiagnostics (bool on);                                 // écrit settings.json ; joue tout de suite
    void uiSetReducedHostList (bool on);                                 // écrit settings.json ; joue au prochain chargement
    bool uiUndo() override;
    bool uiRedo() override;
    const neptune::core::UndoManager& undoManager() const { return undo_; }

    // Instrument du Sampler (palier 1). La description (chemins + réglages de zone) est sauvegardée dans le projet ;
    // le chargement des fichiers se fait sur un thread dédié, jamais sur le thread audio ni sur le thread de message.
    struct InstrumentStatus
    {
        enum class Kind { None, Loading, Loaded, Error } kind = Kind::None;
        std::string name, error;
        int zones = 0, files = 0;       // Loaded : nombre de zones et de fichiers distincts
        int done = 0, total = 0;        // Loading : avancement
        int channels = 0, bitDepth = 0;
        double sampleRate = 0.0, seconds = 0.0, megabytes = 0.0;
    };
    // Piste sélectionnée : toutes les fonctions « instrument » ci-dessous agissent sur elle (sauf mention d'une piste).
    // ---- couche de contrôle (docs/couche-controle.md) : CC MIDI, plus tard OSC et macros ----
    neptune::core::control::Config controlConfig() const { return control_.config(); }
    void uiSetControlConfig (neptune::core::control::Config);          // enregistré avec le projet
    bool ccEnabled() const { return control_.config().settings.ccEnabled; }
    void uiSetCcEnabled (bool on);
    bool oscEnabled() const { return control_.settings().oscEnabled; }
    void uiSetOscEnabled (bool on);
    void uiSetOscAllInterfaces (bool on);
    int oscPort() const { return osc_.port(); }   // 0 : n'écoute pas
    juce::String oscFeedbackDestination() const { return osc_.hasDestination() ? osc_.destinationText() : juce::String(); }
    bool uiSetOscFeedback (const juce::String& hostAndPort);   // « hôte:port » ; vide : arrête le retour. Retourne false si le texte est invalide
    // Remplace les assignations de la piste sélectionnée (si c'est un orgue) par les CC de setBfree, avec les voies de ses claviers.
    bool uiAssignOrganControllers();
    void uiClearControllers();
    neptune::core::control::ControlEngine& control() { return control_; }

    int selectedTrack() const noexcept { return selectedTrack_.load(); }
    void selectTrack (int track); // thread de message ; pas annulable ; n'altère ni l'instrument ni l'audio
    neptune::core::InstrumentDescription instrumentDescription() const { return instrumentDescription (selectedTrack()); }
    neptune::core::InstrumentDescription instrumentDescription (int track) const; // copie
    // Thread de message. `forceReload` : relire les fichiers même si le chemin n'a pas changé. Marque le projet modifié.
    void setInstrumentDescription (neptune::core::InstrumentDescription d, bool forceReload = false) { setInstrumentDescription (selectedTrack(), std::move (d), forceReload); }
    void setInstrumentDescription (int track, neptune::core::InstrumentDescription, bool forceReload = false);
    // Même chose, mais annulable : passe par l'historique (une seule entrée par geste si `mergeKey` est non vide et que
    // uiBeginGesture/uiEndGesture entourent le geste). À utiliser pour toute modification venant de l'utilisateur.
    void uiEditInstrument (neptune::core::InstrumentDescription, std::string name, std::string mergeKey = {});
    void uiEditInstrument (int track, neptune::core::InstrumentDescription, std::string name, std::string mergeKey = {});   // une piste précise
    // Interrupteur direct d'un Groupe du Sampler de la piste (un jeu d'orgue) : le modulateur qui le commande, trouvé ou créé (annulable). Rend son
    // paramètre « Track n Modulator i », ou 0 si le Groupe n'existe pas ou si les 16 modulateurs sont pris. Voir mapping::groupSwitch.
    neptune::core::ParamId ensureGroupSwitch (int track, const std::string& group);
    void uiBeginGesture() { undo_.beginGesture(); }

    // Pistes : type de génération (Sampler / Synthé), pistes actives, patch du synthé.
    neptune::core::TrackType trackType (int track) const;
    bool trackActive (int track) const;
    int activeTrackCount() const;
    bool trackHasContent (int track) const; // instrument chargé ou patch modifié : un changement de type le perdrait (confirmation)
    neptune::core::SynthDescription synthDescription() const { return synthDescription (selectedTrack()); }
    neptune::core::SynthDescription synthDescription (int track) const; // copie
    // Module FM (type de piste FM) : description de la piste, édition annulable, remplacement par un patch importé.
    neptune::core::fm::FmDescription fmDescription() const { return fmDescription (selectedTrack()); }
    neptune::core::fm::FmDescription fmDescription (int track) const;
    void uiEditFm (neptune::core::fm::FmDescription, std::string name, std::string mergeKey = {}, bool presetChange = false);
    // Type Orgue : canaux des claviers, split, transposition (les tirettes et réglages sont des paramètres de la piste).
    neptune::core::organ::OrganDescription organDescription() const { return organDescription (selectedTrack()); }
    neptune::core::organ::OrganDescription organDescription (int track) const;
    void uiEditOrgan (neptune::core::organ::OrganDescription, std::string name, std::string mergeKey = {});
    // Édition du patch de la piste sélectionnée : annulable, fusionnée par geste.
    // `presetChange` : changement de preset (les notes en cours gardent l'ancien son : capture par voix).
    void uiEditSynth (neptune::core::SynthDescription, std::string name, std::string mergeKey = {}, bool presetChange = false);

    // ---- presets de synthé (PresetHost : agit sur la piste sélectionnée, si elle est de type Synthé) ----
    neptune::core::preset::Library& library() override { return *presetLibrary_; }
    std::string currentPresetId() const override;
    const neptune::core::preset::Preset* currentPreset() const override;
    std::uint32_t presetRevision() const override { return presetRevision_.load(); }
    void touchPresets() override { ++presetRevision_; }
    bool loadPreset (const std::string& id) override;
    bool stepPreset (int direction) override;
    bool isModified() const override;
    bool canSaveCurrent() const override;
    bool saveCurrent (std::string* error = nullptr) override;
    void attachFacade (neptune::core::preset::Preset&, int track) const;
    bool overwritePreset (const std::string& id, std::string* error = nullptr) override;
    bool saveCurrentAs (const std::string& name, const std::string& type, neptune::core::preset::Preset* out = nullptr,
                        std::string* error = nullptr) override;
    bool saveConfig (const neptune::core::preset::LibraryConfig&) override;
    const neptune::core::preset::LibraryConfig& presetConfig() const override { return presetLibrary_->config(); }
    bool presetsAvailable() const override
    {
        const auto t = trackType (selectedTrack());
        return t == neptune::core::TrackType::Synth || t == neptune::core::TrackType::Fm || t == neptune::core::TrackType::Organ;
    }
    bool acceptsPreset (const neptune::core::preset::Preset& p) const override;
    std::vector<std::string> presetKeys() const override;
    // Change le type de la piste : la configuration d'instrument est perdue (spec §1), les réglages de bande sont conservés.
    // Vide l'historique d'annulation.
    void uiSetTrackType (int track, neptune::core::TrackType);
    // Active la première piste libre (un synthé par défaut) et la sélectionne ; rend son indice, ou -1 si les 16 sont actives.
    int uiAddTrack();
    // Export d'une piste complète (état de la piste + valeurs de ses réglages) et import dans une NOUVELLE piste (rend son indice, -1 si
    // les 16 pistes sont prises). L'import vide l'historique d'annulation ; le canal MIDI de la nouvelle piste est conservé.
    neptune::core::trackexport::Bundle exportTrackBundle (int track) const;
    // Routeur de notes (accouplements, divisions de clavier) : réglage global, enregistré avec le mixeur, annulable.
    neptune::core::router::Config routerConfig() const;
    // Mode console (voir MixerState.h) : enregistré avec le projet et les presets globaux, pas annulable.
    neptune::core::ConsoleMode consoleMode() const;
    // Façade globale (voir MixerState.h) : un livre de pages libres, enregistré avec le projet et les presets globaux, annulable.
    neptune::core::facade::Facade globalFacade() const;
    void uiEditGlobalFacade (neptune::core::facade::Facade, std::string name, std::string mergeKey = {});
    void uiSetConsoleMode (bool enabled, int track);
    void uiEditRouter (neptune::core::router::Config, std::string name, std::string mergeKey = {});
    // Combinaisons d'orgue (voir Combinations.h) : la bibliothèque (généraux, divisionnels, séquence) est enregistrée avec le projet et annulable ;
    // l'état de jeu (banque courante, touche Set, position du séquenceur) est propre à la session. Tout se passe sur le fil de message.
    neptune::core::combi::Library combinations() const;
    void uiEditCombinations (neptune::core::combi::Library, std::string name, std::string mergeKey = {});
    struct CombiStatus
    {
        int bank = 0;               // 0..9
        bool setArmed = false;
        int sequencePosition = -1;  // -1 : avant le premier pas
        int sequenceLength = 0;
        int lastKey = -1;           // clef du dernier général rappelé (generalKey), -1 : aucun
    };
    CombiStatus combiStatus() const;
    // Les scènes d'une banque pour la grille de la Façade (sans copier toute la bibliothèque) : les `count` premières, nom et texte compris.
    struct SceneCell { bool used = false; std::string name, note, note2; };
    std::vector<SceneCell> sceneCells (int bank, int count) const;
    neptune::core::combi::Result uiCombiScene (int piston);                                  // rappelle la scène `piston` de la banque courante (l'enregistre si Set est armé)
    bool uiCombiSetText (int generalKeyValue, std::string name, std::string note, std::string note2);   // nom et deux lignes de texte d'une scène existante (annulable)
    // Dernier rappel qui a ignoré des inserts (emplacements en insertSlotKey) ; `serial` change à chaque fois (fil de message seulement).
    struct SceneNotice { std::uint32_t serial = 0; std::vector<int> skippedSlots; };
    const SceneNotice& sceneNotice() const noexcept { return sceneNotice_; }
    bool combiArmed() const noexcept { return combi_.armed(); }   // touche Set armée : le prochain piston mémorise (aussi pour les pistons « instantané » de la Façade)
    void uiCombiArm (bool armed) noexcept { combi_.arm (armed); }
    bool uiCombiSequenceInsert (int generalKeyValue);   // insère le général après le pas courant du pas à pas (annulable)
    bool uiCombiSequenceDeleteCurrent();                // supprime le pas courant
    void uiCombiSetBank (int bank) noexcept { combi_.setBank (bank); }   // banque courante 0..9 (commune aux généraux et aux divisionnels)
    // Exécute une commande 1..26 (voir Combinations.h). `track` : le clavier visé par un divisionnel. Un rappel est une seule entrée d'annulation.
    neptune::core::combi::Result uiCombiCommand (int command, int track);
    // Changement de programme MIDI (voir Combinations.h) : 0..9 général, 10..19 divisionnel du clavier `track`, 20..29 banque.
    neptune::core::combi::Result uiCombiProgramChange (int program, int track);
    // La piste active qui écoute le canal MIDI `channel` (0..15) : celle dont l'écoute est la plus étroite ; -1 si aucune.
    int trackForMidiChannel (int channel) const;
    // Voyant d'un piston : vrai si l'état courant est celui de la mémoire (généraux et divisionnels seulement).
    bool combiLit (int command, int track) const;
    // Preset global : l'état du projet sans ce qui est propre à l'instance (identifiant, verrou de profil, assignations CC / OSC).
    // Le chargement remplace tout le mixeur et les valeurs des réglages (comme ouvrir un autre projet) et vide l'historique d'annulation.
    std::string globalStateText() const;
    bool uiLoadGlobalState (const std::string& stateText, std::string* error = nullptr);
    int uiImportTrack (neptune::core::trackexport::Bundle);
    // Importe un orgue GrandOrgue (fichier .organ déjà sur disque) : un clavier = une piste Sampler sur des pistes libres consécutives, routeur et combinaisons fusionnés.
    // `streamFromDisk` : les échantillons sont lus sur le disque après leur début (orgues trop gros pour la mémoire).
    // `excludedPerspectives` : perspectives (positions de micro, voir organimport::listPerspectives) dont les rangs ne sont pas chargés.
    neptune::core::organimport::Result uiImportOrgan (const std::filesystem::path& organFile, bool streamFromDisk = false, const std::vector<std::string>& excludedPerspectives = {});
    // Mémoire que l'orgue occuperait chargé en entier (octets, en-têtes des fichiers seulement) et mémoire dont on dispose pour lui ; 0 = inconnu.
    struct OrganMemory { std::uint64_t needed = 0, headroom = 0; };
    OrganMemory uiEstimateOrganMemory (const std::filesystem::path& organFile, const std::vector<std::string>& excludedPerspectives = {});
    // Import d'un instrument SFZ dans une nouvelle piste Sampler : AddTrack la met dans la première piste libre (ou la piste de départ restée vide), ClearMixer supprime
    // d'abord toutes les autres pistes. L'historique d'annulation est vidé. Erreur « no_free_track » si les 16 pistes sont prises.
    enum class SfzPlacement { AddTrack, ClearMixer };
    neptune::core::sfzimport::Result uiImportSfz (const std::filesystem::path& sfzFile, SfzPlacement placement);
    // Importe un preset d'une banque SoundFont (.sf2 / .sf3) ; `presetIndex` : position dans neptune::core::soundfont::readBank(...).presets. Erreur « no_free_track » si aucune piste libre.
    neptune::core::soundfont::Result uiImportSoundFont (const std::filesystem::path& bankFile, int presetIndex, SfzPlacement placement);
    bool installImportedInstrument (const neptune::core::InstrumentDescription& instrument, const std::string& name, SfzPlacement placement);
    // Remet une piste à zéro et y installe un instrument Sampler (inserts, réglages et instruments des autres types compris). midiChannel < 1 : celui d'une piste neuve.
    void installSamplerTrack (int index, neptune::core::InstrumentDescription instrument, const std::string& name, int midiChannel);
    // Retire une piste (remise à l'état d'origine). Faux s'il n'en reste qu'une. Vide l'historique d'annulation.
    bool uiRemoveTrack (int track);

    // Inserts (modules stock) : 8 emplacements par piste, par bus et sur le Master. La source de vérité est la table des instances
    // vivantes ; leurs valeurs sont recopiées dans l'état à la sauvegarde. Le bypass d'un emplacement est un paramètre de l'hôte.
    using StripKind = neptune::core::StripKind;
    std::string insertType (StripKind, int index, int slot) const; // "" : emplacement vide
    neptune::core::ParamId insertBypassParam (StripKind, int index, int slot) const;
    bool insertBypassed (StripKind, int index, int slot) const;
    // Pose (ou retire, type vide) un module. Remet le bypass à zéro et vide l'historique d'annulation.
    void uiSetInsert (StripKind, int index, int slot, const std::string& type);
    struct InsertHandle
    {
        std::shared_ptr<neptune::core::InsertInstance> instance; // garde le module vivant (vu-mètres, paramètres)
        std::unique_ptr<ParamHost> host;                          // édition annulable des paramètres du module
    };
    InsertHandle openInsert (StripKind, int index, int slot); // instance nulle si l'emplacement est vide
    // Paramètres des modules en insert d'une piste qu'une route de modulation peut viser : (destination, nom), voir modulation::insertDest.
    std::vector<std::pair<neptune::core::ParamId, std::string>> insertModulationTargets (int track) const;
    // Nom lisible d'une destination de modulation : un paramètre de Neptune ou d'un insert de la piste ; vide si elle n'existe plus.
    std::string modulationDestName (int track, neptune::core::ParamId dest) const;
    // ModulationHost (onglet MOD) : la matrice de la piste sélectionnée du mixeur, enregistrée comme un réglage de bande annulable.
    int modTrack() const noexcept override { return selectedTrack(); }
    neptune::core::modulation::Matrix modMatrix (int track) const override { return trackSettings (track).modulation; }
    void modEdit (int track, neptune::core::modulation::Matrix matrix) override
    {
        auto strip = trackSettings (track);
        strip.modulation = std::move (matrix);
        uiEditTrack (track, std::move (strip), "Modulation", "modulation");
    }
    void modBeginGesture() override { uiBeginGesture(); }
    void modEndGesture() override { uiEndGesture(); }
    neptune::core::ParameterSet& modParameters() override { return params_; }
    std::string modDestName (int track, neptune::core::ParamId dest) const override { return modulationDestName (track, dest); }
    std::vector<std::pair<neptune::core::ParamId, std::string>> modInsertTargets (int track) const override { return insertModulationTargets (track); }
    // Réduction de gain (dB, >= 0) des compresseurs insérés sur une bande (le plus fort de ceux qui ne sont pas en bypass), par canal.
    std::pair<float, float> compressorReduction (StripKind, int index) const;

    // Pistes parallèles (spec §1) : jusqu'à 32, en arbre. `parent` : 0..15 = piste, 16..47 = piste parallèle (16 + indice).
    bool parallelActive (int p) const;
    neptune::core::mixer::ParallelSettings parallelSettings (int p) const;
    int parallelDepth (int p) const; // 1 : fille directe d'une piste ; 0 si inactive
    std::vector<int> parallelChildren (int parentId) const;
    // Crée une piste parallèle sous `parent` ; rend son indice, ou -1 (pool plein, parent invalide). Vide l'historique d'annulation.
    int uiAddParallel (int parent);
    // Retire la piste et tous ses descendants ; rend le nombre de pistes retirées. Vide l'historique d'annulation.
    int uiRemoveParallel (int p);
    void uiEditParallel (int p, neptune::core::mixer::ParallelSettings, std::string name, std::string mergeKey = {});

    // Mixeur : réglages de bande (nom, canal MIDI, routage, envois, mode de pan), bus, master. Annulables.
    neptune::core::MixerState mixerState() const; // copie
    neptune::core::mixer::StripSettings trackSettings (int track) const;
    neptune::core::mixer::StripSettings busSettings (int bus) const;
    neptune::core::mixer::MasterSettings masterSettings() const;
    void uiEditTrack (int track, neptune::core::mixer::StripSettings, std::string name, std::string mergeKey = {});
    void uiEditBus (int bus, neptune::core::mixer::StripSettings, std::string name, std::string mergeKey = {});
    void uiEditMaster (neptune::core::mixer::MasterSettings, std::string name, std::string mergeKey = {});
    neptune::core::Engine::Meter trackMeter (int track) const { return engine_.trackMeter (track); }
    // Vélocité du dernier note-on reçu sur un canal MIDI (0..15), 0 quand plus aucune note n'y est tenue (vu-mètres de contrôle des claviers de l'orgue)
    float midiLevel (int channel) const noexcept { return midiLevel_[channel & 15].load (std::memory_order_relaxed); }
    neptune::core::Engine::Meter busMeter (int bus) const { return engine_.busMeter (bus); }
    neptune::core::Engine::Meter parallelMeter (int p) const { return engine_.parallelMeter (p); }
    neptune::core::Engine::Meter masterMeter() const { return engine_.masterMeter(); }
    neptune::core::LoudnessMeter::Reading masterLoudness() const { return engine_.masterLoudness(); } // LUFS du Master
    void resetLoudness() { engine_.resetLoudness(); }

    // Jeu depuis l'interface (clavier de la matrice, bouton d'écoute) : thread de message ; appliqué au bloc audio suivant.
    // Toujours sur la piste sélectionnée.
    void uiNoteOn (int key, float velocity) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::NoteOn, 0, key, velocity, selectedTrack() }); }
    void uiNoteOff (int key) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::NoteOff, 0, key, 0.0f, selectedTrack() }); }
    // Même chose sur une piste précise (le clavier du synthé : une touche tenue doit s'arrêter sur la piste où elle a démarré).
    void uiNoteOnTrack (int track, int key, float velocity, int channel = 0) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::NoteOn, channel, key, velocity, track }); }
    void uiNoteOffTrack (int track, int key, int channel = 0) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::NoteOff, channel, key, 0.0f, track }); }
    // Touche d'un clavier de l'interface (Façade, clavier de l'orgue) : joue la piste ET passe par le routeur de notes (accouplements, divisions), comme du MIDI sur `channel`.
    void uiKeyOn (int track, int key, float velocity, int channel = 0) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::KeyboardOn, channel, key, velocity, track }); }
    void uiKeyOff (int track, int key, int channel = 0) { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::KeyboardOff, channel, key, 0.0f, track }); }
    // source : 0 zone telle quelle, 1 sample brut, 2 région START-END, 3 depuis `fromFrame`, 4 une boucle une fois, 5 une boucle telle qu'entendue (voir SamplerVoices::PreviewSpec)
    void uiPreviewZone (int zoneIndex, std::int64_t fromFrame = -1, int source = 0, int loopIndex = 0)
    {
        engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::PreviewZone, 0, zoneIndex, 0.0f, selectedTrack(), fromFrame,
                               static_cast<std::uint8_t> (source), static_cast<std::uint8_t> (loopIndex) });
    }
    void uiPreviewRelease() { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::PreviewRelease, 0, 0, 0.0f, selectedTrack() }); }
    // Notes jouées (piste, touche, vélocité après courbe) pour le curseur de la matrice ; thread de message.
    bool popNoteTrace (int& track, int& key, int& velocity)
    {
        neptune::core::Engine::NoteTrace t;
        if (! engine_.popNoteTrace (t)) return false;
        track = t.track; key = t.key; velocity = t.velocity;
        return true;
    }
    // Voix qui sonnent (moniteur) : thread de message.
    int readVoices (neptune::core::Engine::VoiceInfo* out, int maxOut) const { return engine_.readVoices (out, maxOut); }
    void uiPanic() { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::Panic, 0, 0, 0.0f, 0 }); }
    // molettes de l'écran d'une piste Synthé ou FM (thread de message) : hauteur -1..+1 (revient au repos au lâcher), modulation 0..1 (CC 1)
    void uiPitchBendTrack (int track, float position) noexcept { engine_.setUiPitchBend (track, position); }
    void uiModWheelTrack (int track, float position) noexcept { engine_.setUiModWheel (track, position); }
    void setVoiceMonitor (bool on) { engine_.setVoiceMonitor (on); }
    std::uint64_t heldKeyWord (int track, int word) const noexcept { return engine_.heldKeyWord (track, word); }
    std::uint64_t heldChannelWord (int track, int channel, int word) const noexcept { return engine_.heldChannelWord (track, channel, word); }
    std::uint64_t heldChannelWordAny (int channel, int word) const noexcept { return engine_.heldChannelWordAny (channel, word); }
    int totalActiveVoices() const noexcept { return engine_.totalActiveVoices(); } // toutes pistes ; lisible depuis l'interface
    // Enregistrement direct de l'entrée audio de l'hôte dans un fichier WAV (dossier de données de Neptune, Edited/Recordings) ; la zone est créée par l'éditeur.
    bool uiStartRecording (std::string& path, std::string* error = nullptr);
    std::int64_t uiStopRecording() { return recorder_.stop(); }
    bool isRecording() const noexcept { return recorder_.recording(); }
    std::int64_t recordedFrames() const noexcept { return recorder_.framesWritten(); }
    double recordingSampleRate() const noexcept { return recorder_.sampleRate(); }
    float recordingPeak() noexcept { return recorder_.peak(); }
    // Écoute libre d'un sample hors zones (voir PreviewSample.h) ; les derniers objets sont gardés vivants ici car les voix pointent dans leur zone temporaire.
    void uiPreviewSample (std::shared_ptr<const neptune::core::PreviewSample> p)
    {
        if (p == nullptr) return;
        recentPreviews_.push_back (p);
        while (recentPreviews_.size() > 8) recentPreviews_.erase (recentPreviews_.begin());
        engine_.publishPreview (selectedTrack(), std::move (p));
        engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::PreviewSample, 0, 0, 0.0f, selectedTrack() });
    }
    void uiPreviewStop() { engine_.postUiEvent ({ neptune::core::Engine::UiEvent::Type::PreviewStop, 0, 0, 0.0f, selectedTrack() }); }
    void uiEndGesture() { undo_.endGesture(); }
    InstrumentStatus instrumentStatus() const { return instrumentStatus (selectedTrack()); }
    InstrumentStatus instrumentStatus (int track) const;
    std::string loadSummary() const;
    std::string activitySummary() const;   // par piste Sampler : zones adoptées par le moteur, notes reçues, voix qui sonnent   // pistes en chargement (avancement) ou en erreur ; vide quand tout est prêt
    // Voix éteintes faute de données du disque (streaming) depuis le chargement du plugin.
    std::uint32_t streamUnderruns() const noexcept { return engine_.streamUnderruns(); }
    // Change à chaque modification (instrument de n'importe quelle piste, réglages du mixeur, sélection) : l'interface compare
    // ce nombre au lieu de comparer tout l'état.
    std::uint32_t instrumentRevision() const noexcept { return revision_.load(); }

    // Taille mémorisée de la modale des potards (par type de terminal), enregistrée dans ui_state.json.
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return uiState_.modalSize (t); }
    void setModalSize (neptune::core::uistate::Terminal, neptune::core::uistate::Size) override;
    // Dernier gabarit de nom utilisé à l'import (JSON), mémorisé dans ui_state.json.
    std::string lastImportTemplate() const { return uiState_.lastImportTemplate; }
    void setLastImportTemplate (const std::string&);

    // Extensions CLAP (clap-juce-extensions) : notes au dialecte CLAP avec note_id, expressions de note, fin de note.
    bool supportsNoteDialectClap (bool isInput) override { return isInput; }
    bool supportsDirectEvent (uint16_t spaceId, uint16_t type) override;
    void handleDirectEvent (const clap_event_header_t* event, int sampleOffset) override;
    bool supportsOutboundEvents() override { return true; }
    void addOutboundEventsToQueue (const clap_output_events* out, const juce::MidiBuffer& midi, int sampleOffset) override;

public:
    // Message MIDI envoyé à la main (widget « Message MIDI » de la Façade, fenêtre d'envoi). `targets` : 1 Neptune (injecté comme s'il venait d'un
    // clavier MIDI : routé aux pistes par canal), 2 sortie MIDI de l'hôte. Thread de message ; faux si la file est pleine ou le message vide.
    bool postMidi (const std::vector<std::uint8_t>& bytes, int targets);

private:
    struct MidiPacket
    {
        std::uint16_t size = 0;
        std::uint8_t targets = 0;
        std::array<std::uint8_t, 1024> bytes {};
    };
    std::array<MidiPacket, 64> midiPackets_ {};       // file du thread de message vers le thread audio
    juce::AbstractFifo midiFifo_ { 64 };
    std::array<MidiPacket, 64> hostMidi_ {};          // thread audio : à envoyer à l'hôte à la fin du bloc
    int numHostMidi_ = 0;

private:
    std::vector<std::shared_ptr<const neptune::core::PreviewSample>> recentPreviews_;   // thread de message
    neptune::core::AudioRecorder recorder_;
    static BusesProperties makeBuses (int numOutputs);

    void seedHostParameters (const std::vector<std::pair<neptune::core::ParamId, double>>&);
    bool notifyHostOnSeed_ = true;       // faux pendant le chargement d'un état (publishSynthPatch)
    std::atomic<float> midiLevel_[16] {};
    int heldPerChannel_[16] {};   // fil audio seulement
    neptune::core::ParameterSet params_; // déclaré avant le moteur : les paramètres survivent à tout ce qui les lit
    neptune::core::Engine engine_;
    neptune::core::InstanceProfile profile_;
    neptune::core::UndoManager undo_; // thread message uniquement
    mutable juce::CriticalSection instrumentLock_;
    neptune::core::MixerState mixer_;                                   // protégé par instrumentLock_
    neptune::core::combi::Controller combi_;                            // état de jeu des combinaisons : fil de message seulement
    // Glissement des valeurs continues rappelées par une scène (fil de message seulement ; avancé par controlTick, environ 30 fois par seconde).
    struct Glide
    {
        neptune::core::ParameterSet* set = nullptr;
        std::shared_ptr<neptune::core::InsertInstance> keep;   // garde vivant le module d'un insert
        std::size_t index = 0;
        double start = 0.0, target = 0.0, startMs = 0.0, durationMs = 1.0;
        juce::AudioProcessorParameter* host = nullptr;         // paramètre d'hôte à prévenir (jeu principal seulement)
    };
    std::vector<Glide> glides_;
    void startGlide (neptune::core::ParameterSet&, std::shared_ptr<neptune::core::InsertInstance> keep, std::size_t index, double target, int milliseconds, juce::AudioProcessorParameter* host);
    void glideTick();
    SceneNotice sceneNotice_;
    neptune::core::combi::Result combiExecute (const std::function<neptune::core::combi::Outcome (neptune::core::combi::Library&)>&);
    // Changements de programme MIDI reçus par le fil audio (canal << 8 | programme), consommés sur le fil de message (voir controlTick).
    juce::AbstractFifo combiProgramFifo_ { 64 };
    std::array<std::atomic<int>, 64> combiPrograms_ {};
    void drainCombinationPrograms();
    std::array<InstrumentStatus, neptune::core::mixer::numTracks> status_; // protégé par instrumentLock_
    std::atomic<int> selectedTrack_ { 0 };
    std::atomic<std::uint32_t> revision_ { 0 };
    neptune::core::uistate::UiState uiState_;
    std::filesystem::path uiStateFile_;
    int probeTracks_ = 0;
    int probeOutputs_ = 0;
    HostProbe hostProbe_;

    void autosaveNow();
    class InstrumentLoader;
    void adoptMixerFromState (const std::optional<neptune::core::MixerState>&);
    class InsertHost;
    class SceneInserts;                                         // accès des scènes aux paramètres des inserts
    static int insertFlatIndex (StripKind, int index) noexcept;
    using InsertRow = std::array<std::shared_ptr<neptune::core::InsertInstance>, neptune::core::mixer::numInserts>;
    std::array<InsertRow, neptune::core::mixer::numTracks + neptune::core::mixer::numBuses + 1 + neptune::core::mixer::numParallels> insertInstances_; // protégé par instrumentLock_
    void dropParallel (int p);                                  // arrête une piste parallèle retirée : moteur, inserts, paramètres
    std::set<std::string> otherTracksPaths (int track) const;
    void setStatus (int track, InstrumentStatus);
    void pushMixerSettingsToEngine();
    void publishSynthPatch (int track, const neptune::core::SynthDescription&, bool freezeRunningVoices = false);
    void publishFmDescription (int track, const neptune::core::fm::FmDescription&);
    void publishOrganDescription (int track, const neptune::core::organ::OrganDescription&);
    void setTrackPresetId (int track, const std::string& id);
    std::unique_ptr<neptune::core::preset::Library> presetLibrary_;
    std::atomic<std::uint32_t> presetRevision_ { 1 };
    void resetTrackParameters (int track, bool includeMixer); // valeurs d'origine (instrument ; fader, pan, mute, solo, envois si includeMixer)
    std::string currentStateText() const; // état complet tel qu'écrit dans le projet et dans l'autosave
    void adoptStateIdentity (const std::string& idFromState);

    neptune::core::settings::GlobalSettings settings_;
    std::filesystem::path settingsFile_;
    std::unique_ptr<neptune::core::AutosaveStore> autosave_;
    std::unique_ptr<InstrumentLoader> loader_; // après engine_ : détruit avant lui
    mutable juce::CriticalSection recoveryLock_;
    std::optional<neptune::core::RecoveredState> recovered_; // protégé par recoveryLock_

    struct AutosaveTimer : juce::Timer
    {
        std::function<void()> onTick;
        void timerCallback() override { if (onTick) onTick(); }
    } autosaveTimer_;

    // Couche de contrôle : le moteur est lu par le thread audio (CC) ; le relais ci-dessous (thread de message, ~30 Hz) exécute les actions
    // demandées et prévient l'hôte des paramètres modifiés par une source externe.
    neptune::core::control::ControlEngine control_;
    std::vector<float> hostIn_[2];   // copie de l'entrée audio de l'hôte (pistes « Entrée audio »)
    std::vector<int> hostIndex_;   // indice du paramètre du cœur -> indice de l'hôte (-1 : non déclaré à l'hôte, liste réduite)
    // Le paramètre d'hôte qui porte un réglage pendant un geste d'édition : le sien, sinon (liste réduite) celui de la macro qui le pilote seule, sans réglage de plage.
    juce::AudioProcessorParameter* hostParamForEdit (neptune::core::ParamId id, std::size_t index) const
    {
        if (auto* hp = hostParam (index)) return hp;
        const auto it = singleMacro_.find (id);
        return it == singleMacro_.end() ? nullptr : hostParam (macroFirst_ + static_cast<std::size_t> (it->second));
    }
    std::unordered_map<std::string, int> singleInsertMacro_;           // clé de réglage d'insert -> macro, quand une seule macro le pilote en pleine course
    static std::string insertKey (const neptune::core::control::Target&);
    void setEngineInsert (StripKind, int index, int slot, std::shared_ptr<const neptune::core::InsertInstance>);   // engine_.setInsert + signale le changement à la couche de contrôle
    std::atomic<std::uint32_t> insertEpoch_ { 0 };
    std::uint32_t seenInsertEpoch_ = 0;
    int namesRefreshTicks_ = 0;                                        // thread de message : nombre de tics avant de renommer à nouveau les macros
    std::unordered_map<neptune::core::ParamId, int> singleMacro_;   // réglage -> macro, quand une seule macro le pilote en pleine course (thread de message)
    juce::AudioProcessorParameter* hostParam (std::size_t index) const
    {
        return index < hostIndex_.size() && hostIndex_[index] >= 0 ? getParameters()[hostIndex_[index]] : nullptr;
    }
    std::array<double, neptune::core::control::kMacros> macroSeen_ {};   // thread audio : dernière valeur appliquée de chaque macro
    std::size_t macroFirst_ = 0;                                         // indice du cœur de la macro 1 (les 32 sont consécutives)
    std::atomic<bool> macroResync_ { true };                             // vrai : au prochain bloc, mémoriser sans appliquer (chargement d'un état)
    OscServer osc_ { control_, params_ };
    void syncOsc();   // thread de message : démarre, arrête ou redémarre le serveur selon les réglages
    neptune::core::control::Settings oscApplied_;
    void feedbackTick();                          // thread de message : envoie les paramètres modifiés au destinataire du retour OSC
    std::vector<std::uint32_t> feedbackVersions_; // dernière version envoyée de chaque paramètre ; vide : retour inactif
    std::vector<std::size_t> feedbackPending_;
    std::size_t feedbackHead_ = 0;
    bool oscRunning_ = false;
    AutosaveTimer controlTimer_;
    void controlTick();
    int declaredLatency_ = 0, latencyTicks_ = 0;   // latence annoncée à l'hôte (compensation de latence du moteur, profil Studio)

    // Notes terminées à signaler à l'hôte (NOTE_END). Écrit et lu sur le thread audio uniquement.
    struct PendingNoteEnd { std::int32_t noteId; std::int16_t port, channel, key; };
    std::array<PendingNoteEnd, 256> pendingEnds_ {};
    int numPendingEnds_ = 0;

    // Notes CLAP reçues avant le bloc, avec leur instant dans le bloc ; vidées par processBlock. Thread audio uniquement.
    struct QueuedNote { int offset = 0; std::uint16_t type = 0; clap_event_note note {}; };
    std::array<QueuedNote, 256> queuedNotes_ {};
    int numQueuedNotes_ = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeptuneProcessor)
};
