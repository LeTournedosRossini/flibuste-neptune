#pragma once
// Modèle de « Carte » v2 : accords, gammes (plages indépendantes des accords), tonalités (marqueurs + tonalité globale), motifs d'Arpège (numéros d'emplacement)
// et mesures réelles du morceau. Tout est en temps de l'hôte (noires). Voir docs/plan-harmonie-arpegiateur.md et docs/modele-motifs-commun.md. Cœur sans JUCE.
#include <cstdint>
#include <string>
#include <vector>

#include "neptune/core/ChordTimeline.h"
#include "neptune/core/SharedChordMap.h"

namespace neptune::core::carte
{
// ---- gammes
struct ScaleItem;
struct ScaleMode
{
    const char* id;        // identifiant stable (enregistré)
    const char* nameFr;
    const char* nameEn;
    std::uint16_t rel;     // notes relatives à la fondamentale, bit n = n demi-tons
};
int scaleModeCount();
const ScaleMode& scaleMode (int index);   // index borné
// Masque absolu (do = 0) de la gamme de fondamentale `root` (0..11).
std::uint16_t scaleMask (int root, int mode);
std::uint16_t scaleItemMask (const ScaleItem&);   // idem pour un élément posé (gamme de la liste ou gamme libre)
constexpr int kModeMajor = 0, kModeMinor = 1;   // tonalités : majeur et mineur naturel

// ---- éléments
constexpr double kMinLength = 0.0625;
constexpr double kForever = 1.0e7;   // longueur d'une tonalité qui dure jusqu'à la fin

// `rel` non nul : gamme libre (venue d'un fichier d'échange, hors des modes de la liste) ; ses notes sont `rel` (bit n = n demi-tons depuis la fondamentale) et son nom `name`.
struct ScaleItem { double start = 0.0, length = 4.0; int root = 0; int mode = 0; bool sharp = false; std::uint16_t rel = 0; std::string name; };
struct KeyItem   { double start = 0.0; int root = 0; bool minor = false; bool sharp = false; };   // dure jusqu'au marqueur suivant
struct PatternItem { double start = 0.0, length = 4.0; int slot = 1; };                           // emplacement d'Arpège 1..16
struct MeasureItem { double start = 0.0, length = 4.0; int num = 4, den = 4; int source = 0; };

struct Score
{
    std::vector<chords::Item> chords;
    std::vector<ScaleItem> scales;
    std::vector<KeyItem> keys;
    std::vector<PatternItem> patterns;
    std::vector<MeasureItem> measures;
    bool hasGlobalKey = false;
    KeyItem globalKey;   // tonalité avant le premier marqueur (et pour tout le morceau s'il n'y a pas de marqueur)
};

// Pose un élément dans une voie : triée, sans chevauchement, le dernier posé l'emporte. Retourne l'indice après tri, ou -1 (valeur invalide).
int placeScale (std::vector<ScaleItem>& v, ScaleItem item);
int placePattern (std::vector<PatternItem>& v, PatternItem item);
int placeKey (std::vector<KeyItem>& v, KeyItem item);   // un marqueur au même début remplace l'ancien
// Noms
std::string scaleName (const ScaleItem& s, chords::Naming naming = chords::Naming::Letters);
std::string keyName (const KeyItem& k, chords::Naming naming = chords::Naming::Letters);

// Pour la carte partagée (toutes les voies).
std::vector<chordmap::Container> toContainers (const Score& score);

// Enregistrement dans l'état du plugin (JSON). `fromJson` lit aussi l'ancien texte des accords seuls (version 1 de Carte).
std::string toJson (const Score& score);
Score fromJson (const std::string& text);

// Morceau d'essai : 8 mesures (la dernière en 3/4), accords simples puis enrichis (septièmes, une basse étrangère), deux gammes, une tonalité globale et un marqueur,
// la voie des motifs qui appelle les emplacements 12 à 15 d'Arpège (ses motifs d'essai). `startBeats` : où il commence dans le projet.
Score demoScore (double startBeats = 0.0);

// ---- import d'un fichier `flibuste-accords` (écrit par Scorsaire)
constexpr int kMaxImportMeasures = 4096, kMaxImportChords = 16384;
struct ImportResult
{
    bool ok = false;
    std::string error;       // message clair pour l'utilisateur (refus, jamais de troncature silencieuse)
    std::string title;
    Score score;             // accords et mesures
    int noChord = 0;         // entrées « pas d'accord » (des trous)
    int unknownQualities = 0;
    // Fichier `flibuste-carte` : les voies présentes dans le fichier (les autres ne sont pas touchées), ce qui n'a pas pu être lu, et les différences à signaler.
    bool hasChords = false, hasMeasures = false, hasScales = false, hasKeys = false, hasPatterns = false;
    int unknownScales = 0, skipped = 0;
    std::vector<std::string> notes;   // par exemple : « l'emplacement 12 s'appelle Essai 1 dans le fichier »
};
// `startBeats` : position dans le projet où commence le morceau (0 du fichier).
ImportResult importChordFile (const std::string& json, double startBeats);

// ---- échange `flibuste-carte` (docs/format-flibuste-carte.md) : accords, gammes, tonalités et appels de motifs, avec les mesures
constexpr int kMaxImportScales = 4096, kMaxImportKeys = 1024, kMaxImportPatterns = 4096;
// Écrit le fichier. `startBeats` : la position du début du morceau dans le projet (soustraite des positions). `patternNames` : nom des motifs 1..16 (facultatif, à titre d'information).
std::string exportCarteFile (const Score&, const std::string& title, double startBeats = 0.0, const std::vector<std::string>& patternNames = {});
// Lit un fichier `flibuste-carte`, ou un fichier `flibuste-accords` (qui ne porte que des accords et des mesures).
ImportResult importCarteFile (const std::string& json, double startBeats, const std::vector<std::string>& patternNames = {});
} // namespace neptune::core::carte
