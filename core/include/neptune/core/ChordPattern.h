#pragma once
// Motif d'accompagnement en degrés d'accord : le format commun avec Scorsaire (docs/modele-motifs-commun.md). Cœur sans JUCE.
//   - Temps en fractions exactes de ronde (les triolets restent justes) ; conversion en noires de l'hôte par `toBeats` (1 ronde = 4 noires).
//   - Notes en degrés de l'accord (« 1 »..« 15 », « b9 », « #11 », « 5+1 », « B » basse, « C » accord entier, préfixe « L » = main gauche, ignorée ici).
//   - Un champ qu'on ne comprend pas est GARDÉ et réécrit tel quel (`extraJson`) : un motif qui passe par Scorsaire ne perd rien.
//   - Les variantes (`alts`) sont gardées sans être jouées (première version).
#include <cstdint>
#include <string>
#include <vector>

namespace neptune::core::pattern
{
constexpr int kFormatVersion = 1;
constexpr int kMaxBars = 16;
constexpr int kMaxHits = 512;

struct Frac
{
    std::int64_t n = 0, d = 1;
    double value() const { return (double) n / (double) d; }
    double toBeats() const { return 4.0 * value(); }   // 1 ronde = 4 noires
};
Frac makeFrac (std::int64_t n, std::int64_t d);   // réduite ; d > 0
int compare (const Frac& a, const Frac& b);
Frac operator+ (const Frac& a, const Frac& b);
Frac operator- (const Frac& a, const Frac& b);

struct Part
{
    enum class Kind { Bass, Chord, Degree };
    Kind kind = Kind::Degree;
    bool leftHand = false;   // lu, jamais utilisé par Arpège
    int degree = 1;          // 1..15
    int accidental = 0;      // -2..2 demi-tons
    bool signedDegree = false;   // « b9 » : intervalle exact ; sinon : note propre de l'accord
    int octave = 0;          // -9..9
    std::string text;        // écriture d'origine
};
// Faux si le texte n'est pas une part valide.
bool parsePart (const std::string& text, Part& out);

struct Hit
{
    Frac at, dur;
    std::vector<Part> parts;
    bool accent = false;
    int vel = 0;           // 0 : absent ; sinon 1..127
    double gate = 0.0;     // 0 : absent ; sinon 0,05..1
    std::string extraJson;   // champs inconnus (objet JSON), vide si aucun
};

struct Pattern
{
    std::string id, name;
    Frac length = makeFrac (1, 1);   // longueur TOTALE (toutes les mesures du motif)
    int bars = 1;
    std::vector<Hit> hits;           // triés par `at`
    std::string extraJson;           // champs inconnus, `alts` comprises
    Frac barLength() const;          // longueur d'une mesure du motif : length / bars
};

struct Result
{
    bool ok = false;
    std::string error;
    std::vector<Pattern> patterns;
};
// Enveloppe « flibuste-motifs » / « scorsaire-chord-patterns » (version <= kFormatVersion), ou un motif seul, ou une liste de motifs.
Result fromJson (const std::string& text);
std::string toJson (const std::vector<Pattern>& patterns, const std::string& from = "arpege");

// Coups d'une mesure du morceau (règle de Scorsaire, `patternHitsForMeasure`) : la mesure k joue la mesure k modulo `bars` du motif ; le motif se répète pour
// remplir la mesure ; un coup qui dépasse la fin de la mesure est coupé à la barre. `measureLength` en fractions de ronde.
std::vector<Hit> hitsForMeasure (const Pattern& p, const Frac& measureLength, int measureIndex);

// ---- degrés
struct ChordContext
{
    int rootMidi = 48;         // fondamentale (note MIDI) du degré 1
    std::uint16_t rel = 0x091; // notes de l'accord relatives à la fondamentale, bit n = n demi-tons (défaut : majeur 0-4-7)
    int bassPc = -1;           // classe de hauteur de la basse (0..11) ou -1 : la fondamentale
};
enum class Absent { Skip, Play };
// Demi-tons au-dessus de la fondamentale du degré (octave comprise), ou -1 si le degré est absent de l'accord et que `absent` = Skip.
int degreeSemitones (const Part& part, std::uint16_t rel, Absent absent);
// Notes MIDI d'une part (vide : rien ne sonne). Basse : sous la fondamentale ; accord : toutes les notes de l'accord (12 premiers demi-tons).
int resolvePart (const Part& part, const ChordContext& chord, Absent absent, int* out, int cap);
} // namespace neptune::core::pattern
