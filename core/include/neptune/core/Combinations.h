#pragma once
// Scènes (ex-combinaisons d'orgue, palier 7, voir docs/specification-combinaisons.md et docs/plan-scenes.md) : des mémoires qui rappellent d'un coup un état de Neptune.
// Ce qu'une scène enregistre dépend du MODÈLE DE CAPTURE choisi au moment d'enregistrer (CaptureSpec) : les jeux d'orgue, la console d'orgue complète, le mixeur, tout, ou un choix libre.
//  - Général : jeux (modulateurs des pistes), tirants d'accouplement du routeur et transpositions de règles, toute la console.
//  - Divisionnel : les jeux d'un seul clavier (une piste).
//  - Annulation générale : repousse tout (jeux, tirants, transpositions à 0).
//  - Séquenceur : liste ordonnée de généraux parcourue pas à pas.
// `Library` est la donnée (dans le projet, voir MixerState) ; `Controller` est l'état de jeu (banque courante, touche Set, position du
// séquenceur) et exécute les actions. Les valeurs sont des valeurs réelles de paramètres. Cœur portable, sans JUCE.
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neptune/core/ParamGroups.h"
#include "neptune/core/Parameter.h"

namespace neptune::core::combi
{
constexpr int kBanks = 10;               // banques ; la banque courante est commune aux généraux et aux divisionnels
constexpr int kPistons = 10;             // pistons par banque (numérotés 1 à 10 à l'écran, 0 à 9 ici)
constexpr int kTracks = 16;              // claviers (pistes) qui ont des divisionnels
constexpr int kMaxSequence = 64;         // pas du séquenceur
constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxValues = 8192; // valeurs d'une mémoire (1024 avant les scènes : un modèle « Tout » en demande davantage)

// Clefs de stockage (clairsemé : seules les mémoires enregistrées existent).
constexpr int generalKey (int bank, int piston) noexcept { return bank * kPistons + piston; }
constexpr int divisionalKey (int track, int bank, int piston) noexcept { return (track * kBanks + bank) * kPistons + piston; }
constexpr int numGeneralKeys = kBanks * kPistons;
constexpr int numDivisionalKeys = kTracks * kBanks * kPistons;

struct Memory
{
    bool used = false;                                   // enregistrée (une mémoire sans valeur est valide : console sans jeux)
    std::string name;
    std::vector<std::pair<ParamId, double>> values;      // valeurs réelles
    bool operator== (const Memory& o) const noexcept { return used == o.used && name == o.name && values == o.values; }
};

// Modèle de capture : ce qu'un enregistrement (Set + piston) mémorise. Les mémoires déjà enregistrées gardent leurs propres valeurs : le modèle ne sert qu'à enregistrer
// et à l'annulation générale (qui remet à leur défaut les paramètres du modèle ; pour les modèles d'orgue, à 0 comme avant).
enum class Template : int { OrganConsole = 0, OrganStops, Mixer, Everything, Custom, kCount };
struct CaptureSpec
{
    Template tpl = Template::OrganConsole;   // le modèle des combinaisons d'orgue d'origine
    unsigned families = 0;                   // Custom : un bit par paramgroups::Family
    unsigned scopes = 0;                     // Custom : un bit par paramgroups::Scope
    bool operator== (const CaptureSpec& o) const noexcept { return tpl == o.tpl && (tpl != Template::Custom || (families == o.families && scopes == o.scopes)); }
    bool operator!= (const CaptureSpec& o) const noexcept { return ! (*this == o); }
};
const char* templateKey (Template) noexcept;                  // « organ_console »... (JSON)
bool templateFromKey (const std::string&, Template& out);

struct Library
{
    CaptureSpec capture;                  // le modèle d'enregistrement courant (dans le projet)
    std::map<int, Memory> generals;       // generalKey
    std::map<int, Memory> divisionals;    // divisionalKey
    std::vector<int> sequence;            // clefs de généraux

    const Memory* general (int bank, int piston) const;
    const Memory* divisional (int track, int bank, int piston) const;
    bool empty() const noexcept { return generals.empty() && divisionals.empty() && sequence.empty() && capture == CaptureSpec {}; }
    bool operator== (const Library& o) const noexcept { return generals == o.generals && divisionals == o.divisionals && sequence == o.sequence && capture == o.capture; }
    bool operator!= (const Library& o) const noexcept { return ! (*this == o); }
};

Library sanitised (Library);                  // bornes, noms et listes tronqués, clefs hors plage retirées
std::string toJson (const Library&);
Library fromJson (std::string_view);          // lecture tolérante

// Édition des généraux et de la séquence (écran Combinaisons). Les clefs sont celles de `generalKey`. Chaque fonction rend faux, sans rien changer, quand elle ne peut pas agir.
bool sequenceAdd (Library&, int generalKeyValue);                 // ajoute un pas à la fin (la mémoire peut être vide pour l'instant) ; faux si la liste est pleine ou la clef invalide
bool sequenceRemove (Library&, int index);
bool sequenceMove (Library&, int index, int delta);               // échange le pas avec son voisin (delta -1 ou +1) ; faux hors de la liste
bool eraseGeneral (Library&, int generalKeyValue);                // efface la mémoire (les pas de la séquence qui la visent restent, ils sont « vides »)
bool renameGeneral (Library&, int generalKeyValue, std::string name);   // la mémoire doit exister ; nom nettoyé et tronqué
bool eraseDivisional (Library&, int divisionalKeyValue);                // les mêmes gestes pour les divisionnels (clef = divisionalKey)
bool renameDivisional (Library&, int divisionalKeyValue, std::string name);

// Paramètres concernés, ceux que le jeu connaît.
//  stopParams : les jeux d'un clavier (les 16 modulateurs de la piste `track`, 0..15).
//  consoleParams : les jeux de tous les claviers, les 32 tirants du routeur et les transpositions de règles.
std::vector<ParamId> stopParams (const ParameterSet&, int track);
std::vector<ParamId> consoleParams (const ParameterSet&);
// Les paramètres qu'enregistre un modèle. `track` < 0 : toute la console (une scène) ; 0..15 : ceux d'une seule piste (un divisionnel, une scène de piste).
std::vector<ParamId> captureParams (const ParameterSet&, const CaptureSpec&, int track = -1);

// Commandes (boutons de Façade : Widget::actionMode = 1 + commande ; actions de la couche de contrôle : CC, notes, OSC).
//  1..10 : piston général 1..10 de la banque courante ; 11..20 : piston divisionnel 1..10 du clavier courant ;
//  21 : annulation générale ; 22 : touche Set (arme / désarme) ; 23 / 24 : pas suivant / précédent du séquenceur ; 25 / 26 : banque suivante / précédente.
constexpr int kCommandGeneral = 1;
constexpr int kCommandDivisional = 11;
constexpr int kCommandCancel = 21;
constexpr int kCommandSet = 22;
constexpr int kCommandNext = 23;
constexpr int kCommandPrevious = 24;
constexpr int kCommandBankNext = 25;
constexpr int kCommandBankPrevious = 26;
constexpr int kNumCommands = 26;
std::string commandKey (int command);                       // « combi_general_3 », « combi_cancel »... (fichiers, couche de contrôle) ; vide hors 1..26
std::string commandLabel (int command);                     // texte court d'un bouton : « G3 », « D3 », « GC », « Set », « Next »...

struct Change
{
    ParamId param = 0;
    double before = 0.0, after = 0.0;
};

// Écrit un paramètre (valeur réelle déjà bornée) : l'adaptateur y met l'annulation et la notification à l'hôte. Sans écrivain, `ParameterSet::setAt`.
using Writer = std::function<void (ParamId, double)>;

// Vrai si le jeu a déjà, pour chaque valeur connue de la mémoire, la valeur qu'elle porte (voyant d'un piston allumé). Une mémoire sans valeur connue : faux.
bool matches (const ParameterSet&, const Memory&);

// Mémorise les valeurs courantes de `ids` (ceux que le jeu ne connaît pas sont ignorés). Le nom est laissé vide.
Memory capture (const ParameterSet&, const std::vector<ParamId>& ids);
// Règle les paramètres de la mémoire ; rend ce qui a réellement changé (valeur bornée au paramètre ; id inconnu : ignoré).
std::vector<Change> recall (ParameterSet&, const Memory&, const Writer& write = {});
// Annulation générale : tout repousser (valeur 0 de chaque paramètre de la console).
std::vector<Change> cancelAll (ParameterSet&, const Writer& write = {});
// Idem pour un modèle : les modèles d'orgue repoussent tout à 0 (jeux, tirants, transpositions) ; les autres remettent les paramètres du modèle à leur valeur par défaut (celle de l'ouverture du logiciel).
std::vector<Change> cancelAll (ParameterSet&, const CaptureSpec&, const Writer& write = {});

enum class Result { Ignored, Empty, Recalled, Stored, Done };   // Done : changement d'état de jeu seul (touche Set, banque)
struct Outcome
{
    Result result = Result::Ignored;
    std::vector<Change> changes;   // Recalled : ce qui a changé
};

class Controller
{
public:
    void setWriter (Writer w) { writer_ = std::move (w); }   // utilisé par recall et l'annulation générale

    int bank() const noexcept { return bank_; }
    void setBank (int bank) noexcept;                 // bornée 0..kBanks-1
    bool nextBank() noexcept;                         // false aux extrémités (pas de bouclage)
    bool previousBank() noexcept;

    // Touche Set : tenue (setHeld) ou armée (arm). Un piston enregistre quand l'une des deux est active ; l'armement se défait après un
    // enregistrement, pas la touche tenue.
    void setHeld (bool held) noexcept { held_ = held; }
    void arm (bool armed) noexcept { armed_ = armed; }
    bool held() const noexcept { return held_; }
    bool armed() const noexcept { return armed_; }
    bool setActive() const noexcept { return held_ || armed_; }

    // Piston `piston` (0..kPistons-1) de la banque courante. Set actif : enregistre l'état courant (le nom existant est conservé).
    Outcome general (Library&, ParameterSet&, int piston);
    Outcome divisional (Library&, ParameterSet&, int track, int piston);
    Outcome cancel (ParameterSet&);                     // modèle d'orgue d'origine
    Outcome cancel (const Library&, ParameterSet&);     // selon le modèle de capture de la bibliothèque

    // Séquenceur : -1 avant le premier pas. Aux extrémités la position reste en place (Ignored). Un général rappelé directement
    // qui figure dans la liste cale la position sur lui.
    // Séquenceur : insère un pas (le général `key`) juste après la position courante (au début s'il n'y en a pas encore), qui devient la position courante ;
    // supprime le pas courant (la position reste sur le suivant, ou sur le dernier). Faux sans effet.
    bool sequenceInsert (Library&, int generalKeyValue);
    bool sequenceDeleteCurrent (Library&);
    int sequencePosition() const noexcept { return seq_; }
    void clampSequence (int length) noexcept { if (seq_ >= length) seq_ = length - 1; }   // après un changement de la liste
    void resetSequence() noexcept { seq_ = -1; }
    Outcome sequenceNext (const Library&, ParameterSet&);
    Outcome sequencePrevious (const Library&, ParameterSet&);

    // Exécute une commande 1..26 (voir plus haut). `track` : le clavier des divisionnels (0..15). Set : arme ou désarme.
    Outcome run (int command, Library&, ParameterSet&, int track);

    // Changement de programme MIDI (0..127) :
    //   0..9   piston général 1..10 de la banque courante ;
    //   10..19 piston divisionnel 1..10 du clavier `track` (la piste dont le canal MIDI est celui du message ; -1 : aucune, ignoré) ;
    //   20..29 choix de la banque 1..10.
    // Le reste est ignoré.
    Outcome programChange (Library&, ParameterSet&, int program, int track = -1);

private:
    Outcome recallKey (const Library&, ParameterSet&, int generalKeyValue);
    Writer writer_;
    int bank_ = 0;
    bool held_ = false, armed_ = false;
    int seq_ = -1;
};
} // namespace neptune::core::combi
