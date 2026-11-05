#pragma once
// Timeline d'accords du futur plugin « Carte » (voir docs/plan-harmonie-arpegiateur.md) : noms d'accords (saisie et affichage), conteneurs placés en temps,
// opérations d'édition qui gardent la suite triée et sans chevauchement, texte d'enregistrement, conversion vers la carte partagée.
// Thread : interface (message). Aucune dépendance à JUCE.
#include <cstdint>
#include <string>
#include <vector>
#include "neptune/core/SharedChordMap.h"

namespace neptune::core::chords
{
enum class Naming { Letters, French };   // C D E... ou Do Ré Mi...

struct Chord
{
    int root = 0;                       // 0..11, do = 0
    int bass = -1;                      // 0..11 ou -1 : pas de basse distincte
    std::uint16_t rel = 0x091;          // notes relatives à la fondamentale, bit n = n demi-tons au-dessus (défaut : majeur 0-4-7)
    bool sharp = false;                 // écriture avec dièse (C# plutôt que Db) pour la fondamentale et la basse

    bool operator== (const Chord& o) const { return root == o.root && bass == o.bass && rel == o.rel && sharp == o.sharp; }
};

// Lit « Dm7 », « C/E », « F#m7b5 », « Sib », « Do », « Ré mineur » n'est pas géré (voir la table des suffixes dans le .cpp). Faux si le texte n'est pas un accord.
bool parseChord (const std::string& text, Chord& out);
// Écrit l'accord ; texte UTF-8. En lettres, le résultat est relu à l'identique par parseChord.
std::string formatChord (const Chord& c, Naming naming = Naming::Letters);
// Les suffixes de qualité qu'on écrit (le premier de chaque famille ; « » = majeur), dans l'ordre de la table : pour le constructeur d'accords.
const std::vector<std::string>& qualitySuffixes();
// Les notes de l'accord comme masque de classes de hauteur absolues (do = 0), basse comprise.
std::uint16_t absoluteMask (const Chord& c);

struct Item
{
    double start = 0.0;    // en temps de l'hôte (noires)
    double length = 4.0;
    Chord chord;
};

// Longueur minimale d'un conteneur, en temps.
constexpr double kMinLength = 0.0625;

// Les opérations gardent `items` triée par début, sans chevauchement (le dernier posé l'emporte : les autres sont coupés ou retirés).
// Retournent l'indice de l'élément concerné après tri, ou -1 si l'opération n'a pas eu lieu.
int place (std::vector<Item>& items, Item item);
int moveItem (std::vector<Item>& items, int index, double newStart);
int resizeItem (std::vector<Item>& items, int index, double newLength);
bool removeItem (std::vector<Item>& items, int index);
// Indice du conteneur qui couvre `beats` (début inclus, fin exclue), ou -1.
int indexAt (const std::vector<Item>& items, double beats);

// Texte d'enregistrement (une ligne par conteneur : début, longueur, accord en lettres) et lecture. La lecture ignore les lignes invalides.
std::string toText (const std::vector<Item>& items);
std::vector<Item> fromText (const std::string& text);

// Pour la publication dans la carte partagée.
std::vector<chordmap::Container> toContainers (const std::vector<Item>& items);
} // namespace neptune::core::chords
