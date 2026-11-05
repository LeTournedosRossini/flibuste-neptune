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
constexpr int kModeMajor = 0, kModeMinor = 1;   // tonalités : majeur et mineur naturel

// ---- éléments
constexpr double kMinLength = 0.0625;
constexpr double kForever = 1.0e7;   // longueur d'une tonalité qui dure jusqu'à la fin

struct ScaleItem { double start = 0.0, length = 4.0; int root = 0; int mode = 0; bool sharp = false; };
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
};
// `startBeats` : position dans le projet où commence le morceau (0 du fichier).
ImportResult importChordFile (const std::string& json, double startBeats);
} // namespace neptune::core::carte
