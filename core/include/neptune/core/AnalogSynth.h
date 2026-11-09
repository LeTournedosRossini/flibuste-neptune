#pragma once
// AnalogSynth (nom provisoire) : le synthé soustractif de Neptune (Synth.h / SynthVoices.h) en plugin instrument indépendant.
// Cœur portable : le plugin CLAP n'ajoute que la fine couche JUCE (adapter-juce/src/AnalogSynthPlugin.*).
//
//  - Identifiants de paramètres : ceux de la piste 0 de Neptune (synthParam(0, ...), samplerParam(0, ...), volume 1000, pan 1001),
//    ce qui permet à la vue Synthé d'être la même que dans Neptune.
//  - AnalogSynthEngine : voix + patch + volume / pan propres ; les notes de l'interface passent par une file sans verrou.
//  - État du plugin : JSON versionné ; préset : Preset.h avec `module` = "analogsynth" et `state` = description du synthé.
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neptune/core/HandoffSlot.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/Modulation.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/Preset.h"
#include "neptune/core/Synth.h"
#include "neptune/core/SynthVoices.h"

namespace neptune::core::analogsynth
{
constexpr const char* moduleId = "analogsynth";
constexpr ParamId volume = probe::trackParam (0, probe::kVolume); // dB, -inf..+6
constexpr ParamId pan = probe::trackParam (0, probe::kPan);       // -100..+100

// Déclare tous les paramètres du plugin (volume, pan, gain, accordage, filtres, 15 paramètres du synthé). Hors temps réel.
bool addParams (ParameterSet& set, std::string* error = nullptr);

// Clé de preset <-> identifiant de paramètre ("osc1.level", "filter1.cutoff", "output.volume"...). 0 / "" si inconnu.
struct KeyedParam { const char* key; ParamId id; };
const std::vector<KeyedParam>& keyedParams();
ParamId paramIdForKey (std::string_view key) noexcept;
constexpr const char* glideKey = "synth.glide"; // réglage de la description (non automatisable) traité comme un paramètre de preset

// Défauts de capture du plugin : volume et pan de sortie, glissando : libres ; tout le reste capturé.
preset::LibraryConfig defaultConfig();

// Matrice de départ des contrôleurs MIDI courants, pour un plugin neuf et pour un preset qui n'en porte pas : molette (CC 1), brillance (CC 74)
// et pression de canal ouvrent le filtre 1, résonance (CC 71) sur la résonance du filtre 1. Tous partent de zéro (un CC jamais reçu ne change rien).
modulation::Matrix defaultMidiMatrix();

// ---- enveloppes, glissando, équilibre des filtres : paramètres <-> description ----
// Le moteur lit ces réglages dans les paramètres de la piste (automatisables) ; la description (éditeur graphique, JSON, presets)
// porte les mêmes valeurs. Règle : à chaque fois que la description change (édition, preset, chargement d'un état), on
// recopie ses valeurs dans les paramètres (`envelopeParamValues`) ; à chaque fois qu'on la lit pour l'afficher ou l'enregistrer,
// on y remet les valeurs courantes des paramètres (`withLiveParams`) : c'est le travail de AnalogSynthEngine::description() et de
// l'adaptateur de Neptune ; les fonctions d'état et de preset ci-dessous prennent une description déjà à jour.
std::vector<std::pair<ParamId, double>> envelopeParamValues (const SynthDescription&, int track = 0);
// Vrai si les paramètres portent déjà (aux arrondis de la description près) les valeurs de la description : on n'y touche alors pas
// (un état relu puis réenregistré doit rester identique octet pour octet).
bool envelopeParamsMatch (const SynthDescription&, const ParameterSet&, int track = 0);
void seedEnvelopeParams (const SynthDescription&, ParameterSet&, int track = 0);
SynthDescription withLiveParams (SynthDescription, const ParameterSet&, int track = 0);

// ---- état du plugin (getStateInformation) ----
// `modulation` (facultatif) : la matrice de modulation du plugin ; écrite seulement si elle n'est pas vide, de sorte qu'un état sans matrice reste identique octet pour octet.
std::string stateToJson (const ParameterSet&, const SynthDescription&, const std::string& currentPresetId, const modulation::Matrix* modulation = nullptr);
// Retourne faux (sans rien modifier) si le texte n'est pas un état d'AnalogSynth.
bool stateFromJson (std::string_view text, ParameterSet&, SynthDescription&, std::string& currentPresetId, modulation::Matrix* modulation = nullptr);   // `modulation` : vide si l'état n'en porte pas

// ---- presets ----
// Les identifiants de `keyedParams()` sont ceux de la piste 0 ; Neptune applique les mêmes presets à n'importe laquelle de ses
// pistes Synthé : `idForTrack` donne l'identifiant du même réglage sur la piste `track`.
ParamId idForTrack (ParamId track0Id, int track) noexcept;

// Instantané du son courant : les paramètres capturés (selon `cfg`), la description complète en `state`.
// `base` (facultatif) : preset existant dont on garde l'identité, les métadonnées et les exceptions de capture (« Enregistrer »).
preset::Preset capturePreset (const ParameterSet&, const SynthDescription&, const preset::LibraryConfig& cfg,
                              const std::string& name, const std::string& type = {}, const preset::Preset* base = nullptr,
                              int track = 0);
// La description que donnerait le preset (le glissando libre est conservé). Sans effet de bord : sert à publier le patch avant
// de changer les valeurs (capture par voix).
SynthDescription presetDescription (const preset::Preset&, const SynthDescription& current, const preset::LibraryConfig& cfg);
// Écrit les valeurs capturées du preset (les paramètres libres ou absents gardent leur valeur).
void applyPresetValues (const preset::Preset&, ParameterSet&, const preset::LibraryConfig& cfg, int track = 0);
// Les deux ensemble ; retourne faux si le preset n'est pas pour ce module.
bool applyPreset (const preset::Preset&, ParameterSet&, SynthDescription&, const preset::LibraryConfig& cfg, int track = 0);
// Vrai si le son courant diffère du preset (valeurs capturées ou patch).
bool differsFromPreset (const preset::Preset&, const ParameterSet&, const SynthDescription&, const preset::LibraryConfig& cfg,
                        int track = 0);
// Presets livrés avec le plugin (lecture seule, ids stables « factory-analogsynth-NN »).
std::vector<preset::Preset> factoryPresets();

// ---- moteur ----
class AnalogSynthEngine
{
public:
    AnalogSynthEngine();

    ParameterSet& params() noexcept { return params_; }
    const ParameterSet& params() const noexcept { return params_; }
    bool ok() const noexcept { return ok_; }

    void prepare (double sampleRate, int maxBlockSize);

    // --- thread de message ---
    // Construit le patch et le remet au thread audio. `newPreset` : changement de preset -> les notes en cours gardent l'ancien
    // patch et leurs anciennes valeurs (capture par voix) ; faux : retouche du patch, les notes en cours suivent.
    void setDescription (const SynthDescription&, bool newPreset = false);
    // Molette de hauteur (thread audio) : -1..+1, multipliée par la plage du patch (`bendRange` demi-tons) et ajoutée à l'accordage de toutes les voix.
    void setPitchBend (float position) noexcept { bend_ = position < -1.0f ? -1.0f : position > 1.0f ? 1.0f : position; }
    int frozenVoices() const noexcept { return voices_.frozenVoices(); }
    // La description avec les valeurs courantes des paramètres d'enveloppe, de glissando et d'équilibre (voir withLiveParams).
    SynthDescription description() const { return withLiveParams (description_, params_); }
    // Charge un preset avec capture par voix : le nouveau patch est publié AVANT de changer les valeurs, pour que l'audio fige les
    // notes en cours avec les valeurs d'avant (voir SynthVoices::setPatch). Faux si le preset est d'un autre module.
    bool loadPreset (const preset::Preset&, const preset::LibraryConfig&);
    void collect() { patchSlot_.collect(); pruneKeeper(); } // libère les anciens patchs
    // Notes de l'interface (clavier à l'écran, aperçu) : file sans verrou vers le thread audio, un seul producteur.
    void postNoteOn (int key, float velocity) noexcept { post (Event::NoteOn, key, velocity); }
    void postNoteOff (int key) noexcept { post (Event::NoteOff, key, 0.0f); }
    void postPanic() noexcept { post (Event::Panic, 0, 0.0f); }
    void postReleaseAll() noexcept { post (Event::ReleaseAll, 0, 0.0f); }

    // --- thread audio ---
    void noteOn (int channel, int key, float velocity) noexcept
    {
        adoptPatch();
        if (key >= 0 && key < 128) held_[key >> 6].fetch_or (std::uint64_t (1) << (key & 63), std::memory_order_relaxed);
        voices_.noteOn (channel, key, velocity);
    }
    void noteOff (int channel, int key) noexcept
    {
        adoptPatch();
        if (key >= 0 && key < 128) held_[key >> 6].fetch_and (~(std::uint64_t (1) << (key & 63)), std::memory_order_relaxed);
        voices_.noteOff (channel, key);
    }
    void releaseAll() noexcept { clearHeld(); voices_.releaseAll(); }
    void panic() noexcept { clearHeld(); voices_.panic(); }
    // Touches tenues (mot 0 : notes 0..63, mot 1 : 64..127), pour l'affichage du clavier.
    std::uint64_t heldKeyWord (int word) const noexcept { return held_[word & 1].load (std::memory_order_relaxed); }
    // Écrase `left` et `right` avec le rendu (volume et pan appliqués). Aucune allocation.
    void process (float* left, float* right, int numFrames) noexcept;

    int activeVoices() const noexcept { return voices_.activeVoices(); }          // thread audio / tests
    int voiceCount() const noexcept { return voiceCount_.load (std::memory_order_relaxed); } // interface : publié à chaque bloc
    float peakLeft() const noexcept { return peakL_.load (std::memory_order_relaxed); }
    float peakRight() const noexcept { return peakR_.load (std::memory_order_relaxed); }

private:
    struct Event { enum Type : int { NoteOn, NoteOff, Panic, ReleaseAll } type = NoteOn; int key = 0; float velocity = 0.0f; };
    void clearHeld() noexcept { held_[0].store (0, std::memory_order_relaxed); held_[1].store (0, std::memory_order_relaxed); }
    float bend_ = 0.0f;   // position de la molette de hauteur (thread audio)
    void adoptPatch() noexcept { if (patchSlot_.adoptPending()) voices_.setPatch (patchSlot_.current()); } // avant toute note
    void post (Event::Type t, int key, float velocity) noexcept;
    void drain() noexcept;

    ParameterSet params_;
    bool ok_ = true;
    std::array<std::size_t, mixer::kSynthKinds> synthIdx_ {};
    std::array<std::size_t, mixer::kSynthEnvKinds> envIdx_ {};
    std::size_t idxVolume_ = 0, idxPan_ = 0, idxGain_ = 0, idxSemi_ = 0, idxCents_ = 0;
    std::array<std::size_t, 2> idxCutoff_ {}, idxReso_ {};

    SynthDescription description_;
    void pruneKeeper();
    HandoffSlot<SynthPatch> patchSlot_;
    SynthPatchKeeper keeper_;
    SynthVoices voices_;
    float lastGain_ = 1.0f, lastFader_ = 1.0f, lastPanL_ = 1.0f, lastPanR_ = 1.0f;
    std::atomic<float> peakL_ { 0.0f }, peakR_ { 0.0f };
    std::atomic<int> voiceCount_ { 0 };
    std::atomic<std::uint64_t> held_[2] {};

    static constexpr std::size_t kQueue = 256;
    std::array<Event, kQueue> queue_ {};
    std::atomic<std::size_t> head_ { 0 }, tail_ { 0 };
};
} // namespace neptune::core::analogsynth
