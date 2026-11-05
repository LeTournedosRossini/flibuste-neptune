// Motifs en degrés d'accord (format commun avec Scorsaire) : lecture / écriture, champs inconnus gardés, degrés, lecture sur une mesure.
#include <catch2/catch_test_macros.hpp>

#include "neptune/core/ChordPattern.h"

using namespace neptune::core::pattern;

namespace
{
Part part (const char* t)
{
    Part p;
    REQUIRE (parsePart (t, p));
    return p;
}
constexpr std::uint16_t bit (int s) { return (std::uint16_t) (1u << s); }
const std::uint16_t kMaj = bit (0) | bit (4) | bit (7);
const std::uint16_t kMin7 = bit (0) | bit (3) | bit (7) | bit (10);
}

TEST_CASE ("motif : lecture des parts", "[pattern]")
{
    CHECK (part ("B").kind == Part::Kind::Bass);
    CHECK (part ("C").kind == Part::Kind::Chord);
    const auto five = part ("5");
    CHECK (five.degree == 5);
    CHECK (! five.signedDegree);
    const auto b9 = part ("b9");
    CHECK (b9.degree == 9);
    CHECK (b9.accidental == -1);
    CHECK (b9.signedDegree);
    CHECK (part ("#11").accidental == 1);
    CHECK (part ("5+1").octave == 1);
    CHECK (part ("3-2").octave == -2);
    CHECK (part ("L5").leftHand);
    CHECK (part ("bb7").accidental == -2);
    CHECK (part ("15").degree == 15);
    Part p;
    CHECK (! parsePart ("R5", p));
    CHECK (! parsePart ("16", p));
    CHECK (! parsePart ("0", p));
    CHECK (! parsePart ("5+10", p));
    CHECK (! parsePart ("", p));
    CHECK (! parsePart ("x", p));
}

TEST_CASE ("motif : degrés propres à l'accord", "[pattern]")
{
    CHECK (degreeSemitones (part ("1"), kMaj, Absent::Skip) == 0);
    CHECK (degreeSemitones (part ("3"), kMaj, Absent::Skip) == 4);
    CHECK (degreeSemitones (part ("5"), kMaj, Absent::Skip) == 7);
    CHECK (degreeSemitones (part ("7"), kMaj, Absent::Skip) == -1);          // absent : sauté
    CHECK (degreeSemitones (part ("7"), kMaj, Absent::Play) == 11);          // absent : note par défaut
    CHECK (degreeSemitones (part ("3"), kMin7, Absent::Skip) == 3);
    CHECK (degreeSemitones (part ("7"), kMin7, Absent::Skip) == 10);
    CHECK (degreeSemitones (part ("5"), bit (0) | bit (3) | bit (6), Absent::Skip) == 6);                      // diminué : b5
    CHECK (degreeSemitones (part ("7"), bit (0) | bit (3) | bit (6) | bit (9), Absent::Skip) == 9);            // dim7 : bb7
    CHECK (degreeSemitones (part ("5"), bit (0) | bit (4) | bit (8), Absent::Skip) == 8);                      // augmenté : #5
    CHECK (degreeSemitones (part ("3"), bit (0) | bit (5) | bit (7), Absent::Skip) == -1);                     // sus4 : pas de tierce
    CHECK (degreeSemitones (part ("4"), bit (0) | bit (5) | bit (7), Absent::Skip) == 5);
    CHECK (degreeSemitones (part ("5+1"), kMaj, Absent::Skip) == 7);          // l'octave s'ajoute à la résolution, pas ici
}

TEST_CASE ("motif : degrés signés et octaves", "[pattern]")
{
    CHECK (degreeSemitones (part ("b9"), kMaj, Absent::Skip) == 13);
    CHECK (degreeSemitones (part ("#11"), kMaj, Absent::Skip) == 18);
    CHECK (degreeSemitones (part ("b3"), kMaj, Absent::Skip) == 3);           // exact, même sur un accord majeur
    CHECK (degreeSemitones (part ("9"), kMaj, Absent::Play) == 14);
    CHECK (degreeSemitones (part ("9"), kMaj, Absent::Skip) == -1);
    CHECK (degreeSemitones (part ("9"), kMaj | bit (14), Absent::Skip) == 14);
    int notes[16];
    ChordContext c;
    c.rootMidi = 48;
    c.rel = kMaj;
    CHECK (resolvePart (part ("5+1"), c, Absent::Skip, notes, 16) == 1);
    CHECK (notes[0] == 48 + 7 + 12);
    CHECK (resolvePart (part ("3-1"), c, Absent::Skip, notes, 16) == 1);
    CHECK (notes[0] == 48 + 4 - 12);
    CHECK (resolvePart (part ("7"), c, Absent::Skip, notes, 16) == 0);
}

TEST_CASE ("motif : basse et accord entier", "[pattern]")
{
    int notes[16];
    ChordContext c;
    c.rootMidi = 50;   // ré
    c.rel = kMin7;
    CHECK (resolvePart (part ("B"), c, Absent::Skip, notes, 16) == 1);
    CHECK (notes[0] == 38);
    c.bassPc = 5;      // D/F : basse = fa, sous la fondamentale
    CHECK (resolvePart (part ("B"), c, Absent::Skip, notes, 16) == 1);
    CHECK (notes[0] % 12 == 5);
    CHECK (notes[0] < 50);
    c.bassPc = -1;
    CHECK (resolvePart (part ("C"), c, Absent::Skip, notes, 16) == 4);
    CHECK (notes[0] == 50);
    CHECK (notes[3] == 60);
}

TEST_CASE ("motif : fichier, aller-retour sans perte", "[pattern]")
{
    const char* text = R"({
      "format": "scorsaire-chord-patterns", "version": 1, "from": "scorsaire",
      "patterns": [ {
        "id": "ballad", "name": "Ballade", "length": [1,1], "folder": "piano", "voices": { "rh": "led" },
        "alts": [ { "id": "fill", "from": [1,2], "hits": [ { "at": [3,4], "dur": [1,4], "parts": ["C"] } ] } ],
        "hits": [
          { "at": [0,1], "dur": [1,4], "parts": ["B","C"], "stroke": "down", "staccato": true },
          { "at": [1,4], "dur": [1,12], "parts": ["5+1"], "accent": true, "vel": 110, "gate": 0.5, "x": { "arpege": { "k": 3 } } },
          { "at": [5,4], "dur": [1,4], "parts": ["C"] },
          { "at": [1,2], "dur": [1,4], "parts": ["??"] }
        ] } ] })";
    const auto r = fromJson (text);
    REQUIRE (r.ok);
    REQUIRE (r.patterns.size() == 1);
    const auto& p = r.patterns[0];
    CHECK (p.name == "Ballade");
    REQUIRE (p.hits.size() == 2);   // coup hors du motif et coup sans part valide : écartés
    CHECK (p.hits[1].dur.n == 1);
    CHECK (p.hits[1].dur.d == 12);
    CHECK (p.hits[1].accent);
    CHECK (p.hits[1].vel == 110);
    CHECK (p.hits[1].gate == 0.5);
    CHECK (! p.hits[0].extraJson.empty());   // stroke, staccato gardés
    const auto out = toJson (r.patterns);
    const auto again = fromJson (out);
    REQUIRE (again.ok);
    REQUIRE (again.patterns.size() == 1);
    CHECK (again.patterns[0].extraJson == p.extraJson);                 // voices, folder, alts
    CHECK (again.patterns[0].hits[0].extraJson == p.hits[0].extraJson);
    CHECK (again.patterns[0].hits[1].extraJson == p.hits[1].extraJson); // x
    CHECK (out.find ("\"staccato\"") != std::string::npos);
    CHECK (out.find ("\"alts\"") != std::string::npos);
    CHECK (out.find ("\"x\"") != std::string::npos);
}

TEST_CASE ("motif : fichier refusé ou toléré", "[pattern]")
{
    CHECK (! fromJson ("pas du json").ok);
    CHECK (! fromJson (R"({"format":"autre","version":1,"patterns":[]})").ok);
    const auto future = fromJson (R"({"format":"flibuste-motifs","version":99,"patterns":[]})");
    CHECK (! future.ok);   // jamais de perte silencieuse : une version trop récente est refusée
    const auto bare = fromJson (R"({"id":"x","hits":[{"at":[0,1],"dur":[1,4],"parts":["1"]}]})");
    REQUIRE (bare.ok);
    CHECK (bare.patterns.size() == 1);
    const auto bad = fromJson (R"({"id":"x","hits":[{"at":[0,1],"dur":[1,4],"parts":["1"],"vel":500,"gate":9}]})");
    REQUIRE (bad.ok);
    CHECK (bad.patterns[0].hits[0].vel == 0);
    CHECK (bad.patterns[0].hits[0].gate == 0.0);
}

TEST_CASE ("motif : lecture sur une mesure du morceau", "[pattern]")
{
    Pattern p;
    p.length = makeFrac (3, 4);
    for (int k = 0; k < 3; ++k)
    {
        Hit h;
        h.at = makeFrac (k, 4);
        h.dur = makeFrac (1, 4);
        Part pt;
        REQUIRE (parsePart ("1", pt));
        h.parts.push_back (pt);
        p.hits.push_back (h);
    }
    // une mesure de 3/4 dans une mesure de 4/4 recommence au quatrième temps, le coup qui dépasse est absent (début >= fin)
    const auto four = hitsForMeasure (p, makeFrac (1, 1), 0);
    REQUIRE (four.size() == 4);
    CHECK (compare (four[3].at, makeFrac (3, 4)) == 0);
    CHECK (compare (four[3].dur, makeFrac (1, 4)) == 0);

    // coupé à la barre
    Pattern q;
    q.length = makeFrac (1, 1);
    Hit h;
    h.at = makeFrac (3, 4);
    h.dur = makeFrac (1, 1);
    Part pt;
    REQUIRE (parsePart ("5", pt));
    h.parts.push_back (pt);
    q.hits.push_back (h);
    const auto cut = hitsForMeasure (q, makeFrac (1, 1), 0);
    REQUIRE (cut.size() == 1);
    CHECK (compare (cut[0].dur, makeFrac (1, 4)) == 0);

    // deux mesures : la mesure k joue la mesure k modulo 2 du motif
    Pattern two;
    two.length = makeFrac (2, 1);
    two.bars = 2;
    Hit a, b;
    a.at = makeFrac (0, 1);
    b.at = makeFrac (1, 1);
    a.dur = b.dur = makeFrac (1, 4);
    Part one, three;
    REQUIRE (parsePart ("1", one));
    REQUIRE (parsePart ("3", three));
    a.parts.push_back (one);
    b.parts.push_back (three);
    two.hits = { a, b };
    CHECK (hitsForMeasure (two, makeFrac (1, 1), 0)[0].parts[0].text == "1");
    CHECK (hitsForMeasure (two, makeFrac (1, 1), 1)[0].parts[0].text == "3");
    CHECK (hitsForMeasure (two, makeFrac (1, 1), 2)[0].parts[0].text == "1");
    CHECK (compare (hitsForMeasure (two, makeFrac (1, 1), 1)[0].at, makeFrac (0, 1)) == 0);   // compté depuis le début de la mesure
}

TEST_CASE ("motif : fractions exactes (triolets)", "[pattern]")
{
    const auto third = makeFrac (1, 12);
    CHECK ((third + third + third).n == 1);
    CHECK ((third + third + third).d == 4);
    CHECK (compare (third + third + third, makeFrac (1, 4)) == 0);
    CHECK (makeFrac (6, 8).n == 3);
    CHECK (makeFrac (6, 8).d == 4);
    CHECK (makeFrac (1, 4).toBeats() == 1.0);
}

TEST_CASE ("motif : édition de la matrice", "[pattern]")
{
    Pattern p;
    p.length = makeFrac (1, 1);
    const auto at0 = makeFrac (0, 1), at1 = makeFrac (1, 4);
    const auto d = makeFrac (1, 4);
    CHECK (toggleCell (p, at1, d, "3"));
    CHECK (toggleCell (p, at0, d, "1"));
    CHECK (toggleCell (p, at0, d, "5"));         // deux parts dans le même coup
    REQUIRE (p.hits.size() == 2);
    CHECK (compare (p.hits[0].at, at0) == 0);   // trié
    CHECK (p.hits[0].parts.size() == 2);
    CHECK (hasCell (p, at0, "5"));
    CHECK (! toggleCell (p, at0, d, "5"));       // retirée
    CHECK (! hasCell (p, at0, "5"));
    CHECK (! toggleCell (p, at0, d, "1"));       // dernière part : le coup disparaît
    CHECK (p.hits.size() == 1);
    CHECK (! toggleCell (p, makeFrac (2, 1), d, "1"));   // hors du motif
    CHECK (! toggleCell (p, at0, d, "zz"));              // part invalide
    CHECK (! toggleAccent (p, at0));                      // pas de coup : rien
    CHECK (toggleAccent (p, at1));
    CHECK (hasAccent (p, at1));
    CHECK (! toggleAccent (p, at1));
    toggleCell (p, makeFrac (3, 4), makeFrac (1, 2), "1");   // dépasse la nouvelle fin
    setLengthAndBars (p, makeFrac (1, 2), 1);
    REQUIRE (p.hits.size() == 1);                         // celui à 1/4 reste, celui à 3/4 est retiré
    CHECK (compare (p.hits[0].at, at1) == 0);
    setLengthAndBars (p, makeFrac (2, 1), 99);
    CHECK (p.bars == kMaxBars);
    clearHits (p);
    CHECK (p.hits.empty());
}

// ================================================================================================ fichiers écrits par Scorsaire (docs/exemples-scorsaire)

#include <fstream>
#include <sstream>

namespace
{
std::string readExample (const char* name)
{
    std::ifstream in (std::string (NEPTUNE_SOURCE_DIR) + "/docs/exemples-scorsaire/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}

TEST_CASE ("motif : fichier d'export de Scorsaire lu et réécrit sans perte", "[pattern][scorsaire]")
{
    const auto text = readExample ("motifs-scorsaire.json");
    REQUIRE (! text.empty());
    const auto r = fromJson (text);
    REQUIRE (r.ok);
    REQUIRE (r.patterns.size() == 4);
    CHECK (r.patterns[0].id == "ballad");
    CHECK (r.patterns[1].id == "waltz");
    CHECK (compare (r.patterns[1].length, makeFrac (3, 4)) == 0);
    const auto& ex = r.patterns[3];
    REQUIRE (ex.hits.size() == 4);
    CHECK (ex.hits[0].vel == 64);
    CHECK (ex.hits[0].gate == 0.5);
    CHECK (ex.hits[1].accent);
    CHECK (ex.hits[1].vel == 100);
    CHECK (ex.hits[3].gate == 0.3);
    CHECK (! ex.hits[0].extraJson.empty());   // x du coup
    CHECK (! ex.extraJson.empty());           // x du motif

    // réécriture : tout revient identique
    const auto again = fromJson (toJson (r.patterns));
    REQUIRE (again.ok);
    REQUIRE (again.patterns.size() == 4);
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK (again.patterns[i].extraJson == r.patterns[i].extraJson);
        REQUIRE (again.patterns[i].hits.size() == r.patterns[i].hits.size());
        for (size_t k = 0; k < r.patterns[i].hits.size(); ++k)
        {
            const auto &a = again.patterns[i].hits[k], &b = r.patterns[i].hits[k];
            CHECK (a.extraJson == b.extraJson);
            CHECK (a.vel == b.vel);
            CHECK (a.gate == b.gate);
            CHECK (a.accent == b.accent);
            CHECK (compare (a.at, b.at) == 0);
            CHECK (compare (a.dur, b.dur) == 0);
        }
    }

    // une version supérieure à 1 est refusée
    auto future = text;
    const auto pos = future.find ("\"version\": 1");
    REQUIRE (pos != std::string::npos);
    future.replace (pos, 12, "\"version\": 2");
    CHECK (! fromJson (future).ok);
}
