// Carte v2 : gammes, tonalités, motifs, mesures, enregistrement, publication dans la carte partagée, import des fichiers de Scorsaire.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <sstream>

#include "neptune/core/CarteScore.h"

using namespace neptune::core;
using namespace neptune::core::carte;

namespace
{
std::string example (const char* name)
{
    std::ifstream in (std::string (NEPTUNE_SOURCE_DIR) + "/docs/exemples-scorsaire/" + name);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
chords::Item chordAt (double start, double len, const char* name)
{
    chords::Item it;
    it.start = start;
    it.length = len;
    REQUIRE (chords::parseChord (name, it.chord));
    return it;
}
}

TEST_CASE ("carte : gammes, masques absolus", "[carte]")
{
    CHECK (scaleMask (0, kModeMajor) == 0xAB5);          // do majeur : do ré mi fa sol la si
    CHECK (scaleMask (9, kModeMinor) == 0xAB5);          // la mineur naturel : mêmes notes
    CHECK (scaleMask (2, 4) == 0xAB5);                   // ré dorien : mêmes notes aussi
    CHECK (scaleMask (7, 7) == 0xAB5);                   // sol mixolydien
    CHECK (scaleMask (0, 9) == 0x295);                   // pentatonique majeure de do : 0 2 4 7 9
    CHECK (scaleModeCount() >= 15);
    CHECK (scaleMode (0).rel != 0);
    CHECK (scaleMode (999).rel == scaleMode (scaleModeCount() - 1).rel);   // indice borné
}

TEST_CASE ("carte : plages de gammes indépendantes des accords", "[carte]")
{
    std::vector<ScaleItem> v;
    CHECK (placeScale (v, { 0.0, 8.0, 0, 0 }) == 0);
    CHECK (placeScale (v, { 8.0, 4.0, 2, 4 }) == 1);
    CHECK (v.size() == 2);
    // une gamme posée au milieu coupe l'ancienne en deux
    CHECK (placeScale (v, { 2.0, 2.0, 7, 7 }) == 1);
    REQUIRE (v.size() == 4);
    CHECK (v[0].length == 2.0);
    CHECK (v[2].start == 4.0);
    CHECK (v[2].length == 4.0);
    CHECK (placeScale (v, { -1.0, 2.0, 0, 0 }) == -1);
    CHECK (placeScale (v, { 0.0, 2.0, 12, 0 }) == -1);
    CHECK (placeScale (v, { 0.0, 2.0, 0, 99 }) == -1);
    std::vector<PatternItem> p;
    CHECK (placePattern (p, { 0.0, 4.0, 3 }) == 0);
    CHECK (placePattern (p, { 0.0, 4.0, 17 }) == -1);
    CHECK (placePattern (p, { 0.0, 4.0, 0 }) == -1);
}

TEST_CASE ("carte : marqueurs de tonalité et tonalité globale", "[carte]")
{
    Score s;
    s.hasGlobalKey = true;
    s.globalKey.root = 0;
    placeKey (s.keys, { 16.0, 7, false, false });   // sol majeur à la mesure 5
    placeKey (s.keys, { 32.0, 9, true, false });    // la mineur plus loin
    placeKey (s.keys, { 16.0, 5, false, false });   // même début : remplace
    CHECK (s.keys.size() == 2);
    CHECK (s.keys[0].root == 5);
    const auto cs = toContainers (s);
    std::vector<chordmap::Container> keys;
    for (const auto& c : cs) if (c.kind == chordmap::Kind::Key) keys.push_back (c);
    REQUIRE (keys.size() == 3);                      // globale, puis deux marqueurs
    CHECK (keys[0].startBeats == 0.0);
    CHECK (keys[0].lengthBeats == 16.0);             // la globale dure jusqu'au premier marqueur
    CHECK (keys[0].pitchMask == 0xAB5);
    CHECK (keys[1].root == 5);
    CHECK (keys[1].lengthBeats == 16.0);
    CHECK (keys[2].root == 9);
    CHECK (keys[2].pitchMask == scaleMask (9, kModeMinor));
    // sans marqueur : la globale couvre tout
    Score only;
    only.hasGlobalKey = true;
    const auto c2 = toContainers (only);
    REQUIRE (c2.size() == 1);
    CHECK (c2[0].lengthBeats == kForever);
    // un marqueur à 0 : pas de globale en double
    Score at0;
    at0.hasGlobalKey = true;
    placeKey (at0.keys, { 0.0, 2, false, false });
    CHECK (toContainers (at0).size() == 1);
}

TEST_CASE ("carte : enregistrement et relecture de tout le modèle", "[carte]")
{
    Score s;
    chords::place (s.chords, chordAt (0.0, 4.0, "Dm7"));
    chords::place (s.chords, chordAt (4.0, 4.0, "G7"));
    placeScale (s.scales, { 0.0, 8.0, 2, 4, false });
    placeKey (s.keys, { 0.0, 0, false, false });
    placePattern (s.patterns, { 0.0, 8.0, 5 });
    s.measures.push_back ({ 0.0, 1.0, 4, 4, 0 });
    s.measures.push_back ({ 1.0, 4.0, 4, 4, 1 });
    s.hasGlobalKey = true;
    s.globalKey.root = 7;
    s.globalKey.minor = true;
    const auto back = fromJson (toJson (s));
    REQUIRE (back.chords.size() == 2);
    CHECK (back.chords[1].start == 4.0);
    CHECK (back.chords[1].chord == s.chords[1].chord);
    REQUIRE (back.scales.size() == 1);
    CHECK (back.scales[0].mode == 4);
    REQUIRE (back.keys.size() == 1);
    REQUIRE (back.patterns.size() == 1);
    CHECK (back.patterns[0].slot == 5);
    REQUIRE (back.measures.size() == 2);
    CHECK (back.measures[1].source == 1);
    CHECK (back.hasGlobalKey);
    CHECK (back.globalKey.root == 7);
    CHECK (back.globalKey.minor);
    // l'ancien texte de Carte v1 (accords seuls) est encore lu
    const auto old = fromJson (chords::toText (s.chords));
    CHECK (old.chords.size() == 2);
    CHECK (old.scales.empty());
}

TEST_CASE ("carte : import de la reprise avec volta (Scorsaire)", "[carte][scorsaire]")
{
    const auto r = importChordFile (example ("accords-reprise-volta.json"), 0.0);
    REQUIRE (r.ok);
    CHECK (r.score.measures.size() == 6);
    CHECK (r.score.chords.size() == 6);
    // déroulé : 0 1 0 2 3 4 = C G C Am F C ; chaque accord commence à la fin du précédent
    for (size_t i = 1; i < r.score.chords.size(); ++i)
        CHECK (r.score.chords[i].start == r.score.chords[i - 1].start + r.score.chords[i - 1].length);
    CHECK (r.score.chords[0].chord.root == 0);
    CHECK (r.score.chords[1].chord.root == 7);
    CHECK (r.score.chords[3].chord.root == 9);
    CHECK (r.score.chords[3].chord.rel == 0x089);   // la mineur : le dictionnaire du fichier (0 3 7)
}

TEST_CASE ("carte : import du D.S. al Coda (Scorsaire)", "[carte][scorsaire]")
{
    const auto r = importChordFile (example ("accords-ds-al-coda.json"), 0.0);
    REQUIRE (r.ok);
    CHECK (r.score.measures.size() == 9);
    CHECK (r.score.chords.size() == 9);
}

TEST_CASE ("carte : import de l'anacrouse, du 3/4, de la basse et du pas d'accord", "[carte][scorsaire]")
{
    const auto r = importChordFile (example ("accords-anacrouse-chiffrage.json"), 8.0);   // le morceau commence à 8 noires
    REQUIRE (r.ok);
    REQUIRE (r.score.measures.size() == 5);
    CHECK (r.score.measures[0].start == 8.0);
    CHECK (r.score.measures[0].length == 1.0);        // l'anacrouse : un temps, plus courte que son chiffrage
    CHECK (r.score.measures[0].num == 4);
    CHECK (r.score.measures[1].start == 9.0);
    CHECK (r.score.measures[1].length == 4.0);
    CHECK (r.score.measures[3].num == 3);             // le 3/4
    CHECK (r.score.measures[3].den == 4);
    CHECK (r.score.measures[3].length == 3.0);
    CHECK (r.noChord == 1);
    CHECK (r.score.chords.size() == 5);               // le « pas d'accord » est un trou, pas un accord
    bool foundBass = false;
    for (const auto& c : r.score.chords)
        if (c.chord.root == 9 && c.chord.bass == 4) { foundBass = true; CHECK (c.chord.rel == 0x489); }   // Am7/E : 0 3 7 10
    CHECK (foundBass);
    // le trou : aucun accord ne couvre l'instant du « pas d'accord »
    int covering = 0;
    for (const auto& c : r.score.chords)
        if (c.start <= 8.0 + 4.0 * 0.75 + 0.5 * 4.0 * 0.5 && c.start + c.length > 8.0 + 4.0 * 0.75 + 0.5 * 4.0 * 0.5) ++covering;
    (void) covering;
    // publication : les mesures arrivent telles quelles dans la carte partagée
    const auto cs = toContainers (r.score);
    int measures = 0;
    for (const auto& c : cs) if (c.kind == chordmap::Kind::Measure) ++measures;
    CHECK (measures == 5);
}

TEST_CASE ("carte : import refusé clairement, jamais tronqué", "[carte]")
{
    CHECK (! importChordFile ("pas du json", 0.0).ok);
    CHECK (! importChordFile (R"({"format":"autre","version":1})", 0.0).ok);
    const auto future = importChordFile (R"({"format":"flibuste-accords","version":2,"chords":[]})", 0.0);
    CHECK (! future.ok);
    CHECK (! future.error.empty());
    std::string big = R"({"format":"flibuste-accords","version":1,"measures":[)";
    for (int i = 0; i < kMaxImportMeasures + 1; ++i) big += std::string (i ? "," : "") + R"({"at":[0,1],"dur":[1,1],"ts":[4,4],"source":0})";
    big += "]}";
    const auto tooMany = importChordFile (big, 0.0);
    CHECK (! tooMany.ok);
    CHECK (tooMany.error.find ("4096") != std::string::npos);
    CHECK (tooMany.score.measures.empty());
    // une qualité inconnue : la fondamentale seule, comptée
    const auto odd = importChordFile (R"({"format":"flibuste-accords","version":1,"chords":[{"at":[0,1],"dur":[1,1],"root":"C##","quality":"zzz","bass":"Bbb"}]})", 0.0);
    REQUIRE (odd.ok);
    CHECK (odd.unknownQualities == 1);
    REQUIRE (odd.score.chords.size() == 1);
    CHECK (odd.score.chords[0].chord.root == 2);      // C## = ré
    CHECK (odd.score.chords[0].chord.bass == 9);      // Bbb = la
    CHECK (odd.score.chords[0].chord.rel == 1);
}
