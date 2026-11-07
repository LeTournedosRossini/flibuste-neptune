#pragma once
// Couche de contrôle (docs/couche-controle.md) : relie des SOURCES (CC MIDI, OSC, macros du pool) à des CIBLES (un paramètre de
// ParameterSet, ou une action), par une table d'ASSIGNATIONS enregistrée avec le projet. Cœur portable, sans JUCE.
//
// Règles de temps réel (voir le document) : le thread audio ne reçoit que des CC et des macros ; il cherche dans des tableaux directs,
// écrit des valeurs atomiques et pose les actions dans une file sans verrou. Rien n'est alloué, rien n'est verrouillé, l'hôte n'est jamais
// appelé. Les actions, l'apprentissage et le relais vers l'hôte se font sur le thread de message.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "neptune/core/HandoffSlot.h"
#include "neptune/core/Parameter.h"

namespace neptune::core::control
{
constexpr int kMaxBindings = 512;
constexpr int kMacros = 256;          // taille du pool de macros assignables (les emplacements d'automation que voit l'hôte en liste réduite)
constexpr int kMaxAddressLength = 128;

// Pool de macros : kMacros paramètres visibles de l'hôte (« Macro 1 » à « Macro 256 », 0..100 %), identifiants 13000 + indice. Quand une macro
// change (automation de l'hôte, interface, OSC), les assignations dont elle est la source écrivent leurs cibles (ControlEngine::macroChanged).
constexpr ParamId kMacroBase = 13000;
constexpr ParamId macroParamId (int index) noexcept { return kMacroBase + static_cast<ParamId> (index); }
bool addMacroParams (ParameterSet&, std::string* error = nullptr);
// Paramètres de « bande » (volume, pan, mute, solo du master, des pistes, bus et pistes parallèles ; trim, mono et bypass d'inserts du master) et macros : ce que garde
// la liste réduite des paramètres visibles de l'hôte.
bool isBandOrMacroParam (ParamId) noexcept;

enum class SourceKind { CC, Osc, Macro, Nrpn };
// Les 26 commandes de combinaisons d'orgue (voir Combinations.h) suivent les actions de base : valeur = 5 + commande (1..26).
enum class Action { None = 0, PresetNext, PresetPrevious, Panic, TrackNext, TrackPrevious };
constexpr int kNumBaseActions = 6;
constexpr int kNumCombinationActions = 26;
constexpr int kNumActions = kNumBaseActions + kNumCombinationActions;
constexpr Action combinationAction (int command) noexcept { return static_cast<Action> (kNumBaseActions - 1 + command); }
constexpr int combinationCommand (Action a) noexcept { return static_cast<int> (a) >= kNumBaseActions ? static_cast<int> (a) - (kNumBaseActions - 1) : 0; }   // 0 : pas une commande
const char* toString (Action) noexcept;                 // "none" / "preset_next" / "preset_previous" / "panic" / "track_next" / "track_previous" / "combi_general_1"...
bool actionFromString (std::string_view, Action& out);

struct Source
{
    SourceKind kind = SourceKind::CC;
    int channel = 0;          // CC : 0 = toutes les voies, 1..16
    int number = 0;           // CC : 0..127 ; macro : 0..kMacros-1 ; NRPN : 0..16383 (MSB x 128 + LSB), 14 bits, voie comme pour un CC
    std::string address;      // OSC : « /neptune/... »
    bool operator== (const Source& o) const noexcept { return kind == o.kind && channel == o.channel && number == o.number && address == o.address; }
};

std::string sourceLabel (const Source&);       // « CC 74 », « CC 74 ch 2 », « Macro 3 », « OSC /a/b » (indépendant de la langue)

struct Target
{
    bool isAction = false;
    ParamId param = 0;        // si ! isAction
    Action action = Action::None;
    bool operator== (const Target& o) const noexcept { return isAction == o.isAction && param == o.param && action == o.action; }
};

// Pickup : le paramètre ne bouge que lorsque la source passe par sa valeur actuelle (pas de saut).
// Relatifs (encodeurs sans fin, sources CC et paramètres seulement) : la valeur reçue est un incrément ajouté à la valeur du paramètre, 1/127 de la
// course (min..max) par cran. RelativeTwos : 1..63 monte, 65..127 descend (complément à deux) ; RelativeOffset : 64 = repos, au-dessus monte, en dessous
// descend ; RelativeSign : bit 6 = signe, bits 0..5 = amplitude.
enum class Mode { Absolute, Pickup, RelativeTwos, RelativeOffset, RelativeSign };
inline bool isRelative (Mode m) noexcept { return m == Mode::RelativeTwos || m == Mode::RelativeOffset || m == Mode::RelativeSign; }
// Incrément signé (en crans) d'une valeur de CC (0..127) pour un mode relatif.
int relativeDelta (Mode, int value) noexcept;

struct Binding
{
    Source source;
    Target target;
    double lo = 0.0, hi = 1.0;  // sous-intervalle (normalisé) du paramètre visé
    double curve = 1.0;         // exposant : 1 linéaire, > 1 plus de finesse en bas de course, < 1 en haut
    bool invert = false;
    Mode mode = Mode::Absolute;
    bool operator== (const Binding& o) const noexcept
    {
        return source == o.source && target == o.target && lo == o.lo && hi == o.hi && curve == o.curve && invert == o.invert && mode == o.mode;
    }
};

// Réglages globaux. Tout est DÉSACTIVÉ par défaut.
struct Settings
{
    bool ccEnabled = false;          // les contrôleurs MIDI agissent sur les paramètres
    bool oscEnabled = false;         // le serveur OSC écoute
    int oscPort = 9000;              // port de base (le premier libre à partir de lui est pris)
    bool oscAllInterfaces = false;   // écoute sur le réseau local ; sinon seulement sur cette machine (localhost)
    std::string feedbackHost;        // retour d'information OSC (vide : aucun)
    int feedbackPort = 0;
    bool operator== (const Settings& o) const noexcept
    {
        return ccEnabled == o.ccEnabled && oscEnabled == o.oscEnabled && oscPort == o.oscPort && oscAllInterfaces == o.oscAllInterfaces && feedbackHost == o.feedbackHost
            && feedbackPort == o.feedbackPort;
    }
};

struct Config
{
    Settings settings;
    std::vector<Binding> bindings;
    bool operator== (const Config& o) const noexcept { return settings == o.settings && bindings == o.bindings; }
};

Config sanitised (Config);                       // bornes, adresses valides, au plus kMaxBindings assignations
std::string toJson (const Config&);              // UTF-8
Config configFromJson (std::string_view text);   // lecture tolérante : un champ ou une assignation invalide est ignoré

// Valeur normalisée d'un paramètre pour une valeur de source `s` (0..1) : inversion, courbe, puis sous-intervalle.
double mapValue (const Binding&, double s) noexcept;

class ControlEngine
{
public:
    ControlEngine();
    ~ControlEngine();
    ControlEngine (const ControlEngine&) = delete;
    ControlEngine& operator= (const ControlEngine&) = delete;

    // ---- thread de message ----
    // Construit les tables de recherche pour ce jeu de paramètres (taille fixe après la déclaration) et les remet à l'audio. Les
    // assignations qui visent un paramètre inconnu sont gardées dans la configuration mais n'agissent pas.
    void configure (const Config&, const ParameterSet&);
    Config config() const;
    Settings settings() const;                   // les réglages seuls (léger : sans les assignations)
    void collectGarbage();                       // libère les tables remplacées
    void resetPickup();                 // les assignations « reprise » attendent de nouveau le passage de la source (changement de preset)

    void setLearning (bool on) noexcept { learning_.store (on, std::memory_order_relaxed); }
    bool takeLearnedCc (int& channel, int& number) noexcept; // dernier CC reçu pendant l'apprentissage (channel 1..16)
    bool takeLearnedNrpn (int& channel, int& number) noexcept; // dernier NRPN complet reçu pendant l'apprentissage (channel 1..16, number 0..16383)
    bool popAction (Action& out) noexcept;       // action demandée par une source ; à exécuter sur ce thread
    // Indices des paramètres modifiés par une source depuis le dernier appel (à signaler à l'hôte, regroupés). Retourne leur nombre.
    std::size_t drainChanges (std::vector<std::size_t>& out);

    // ---- thread audio (sans allocation ni verrou) ----
    void controlChange (int channel0, int number, int value, ParameterSet&) noexcept;   // channel0 : 0..15
    void macroChanged (int macro, double normalized, ParameterSet&) noexcept;

    // ---- thread réseau (OSC), sans toucher à l'audio ----
    // Valeur reçue pour une adresse (0..1) ; retourne le nombre d'assignations atteintes.
    int oscValue (const std::string& address, double normalized, ParameterSet&) noexcept;
    // Adresses OSC des assignations (pour l'abonnement ou le catalogue).
    std::vector<std::string> oscAddresses() const;
    // Écrit directement un paramètre (sans assignation) : valeur normalisée 0..1, ou réelle si `real`. Le paramètre est signalé comme
    // modifié par une source (relais vers l'hôte). Retourne false si l'identifiant est inconnu ou si la valeur n'est pas finie.
    bool setParameter (ParamId, double value, bool real, ParameterSet&) noexcept;

private:
    struct Runtime;
    void apply (const Runtime&, int item, double s, ParameterSet&) noexcept;
    void nrpnValue (int channel0, int number, int value14, ParameterSet&) noexcept;
    void post (Action) noexcept;
    void markChanged (std::size_t index) noexcept;

    HandoffSlot<Runtime> slot_;                  // vers le thread audio
    std::shared_ptr<const Runtime> shared_;      // pour le thread réseau et la configuration
    mutable std::mutex mutex_;                   // protège shared_ et config_ (jamais pris par l'audio)
    Config config_;
    std::atomic<bool> ccEnabled_ { false };
    std::atomic<bool> learning_ { false };
    std::atomic<std::uint32_t> learned_ { 0 };   // 1 << 24 | canal (1..16) << 8 | numéro ; 0 : rien
    std::atomic<std::uint32_t> learnedNrpn_ { 0 }; // 1 << 24 | canal (1..16) << 16 | numéro de 14 bits ; 0 : rien
    struct NrpnState { int msb = -1, lsb = -1, dataMsb = 0, dataLsb = 0; };
    std::array<NrpnState, 16> nrpn_ {};          // thread audio seulement : sélection et données en cours, par voie

    static constexpr std::size_t kDirtyWords = 512;       // 32768 paramètres au plus
    std::unique_ptr<std::atomic<std::uint64_t>[]> dirty_;

    static constexpr std::size_t kQueue = 64;
    std::array<std::atomic<int>, kQueue> queue_ {};
    std::atomic<std::uint32_t> qHead_ { 0 };
    std::uint32_t qTail_ = 0;
};
} // namespace neptune::core::control
