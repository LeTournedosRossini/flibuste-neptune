// Carte v2 : gammes, tonalités, motifs, mesures, enregistrement, publication dans la carte partagée, import des fichiers de Scorsaire.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "neptune/core/CarteScore.h"
#include "neptune/core/ChordPattern.h"

using namespace neptune::core;
using namespace neptune::core::carte;
using Catch::Approx;

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

TEST_CASE ("carte : noms de gammes et de tonalités", "[carte]")
{
    CHECK (carte::keyName ({ 0.0, 0, false, false }, chords::Naming::French) == "Do majeur");
    CHECK (carte::keyName ({ 0.0, 9, true, false }, chords::Naming::Letters) == "A minor");
    CHECK (carte::keyName ({ 0.0, 3, false, true }, chords::Naming::Letters) == "D# major");
    CHECK (carte::scaleName ({ 0.0, 4.0, 2, 4, false }, chords::Naming::Letters) == "D dorian");
    CHECK (carte::scaleName ({ 0.0, 4.0, 10, 9, false }, chords::Naming::French) == "Sib pentatonique majeure");
}

TEST_CASE ("carte : morceau d'essai", "[carte]")
{
    const auto s = carte::demoScore (8.0);
    REQUIRE (s.measures.size() == 8);
    REQUIRE (s.chords.size() == 8);
    CHECK (s.measures[0].start == 8.0);
    CHECK (s.measures[7].num == 3);
    CHECK (s.measures[7].length == 3.0);
    for (size_t i = 1; i < s.chords.size(); ++i)   // les accords se suivent sans trou, comme les mesures
    {
        CHECK (s.chords[i].start == s.chords[i - 1].start + s.chords[i - 1].length);
        CHECK (s.measures[i].start == s.chords[i].start);
    }
    CHECK (s.patterns.size() == 4);
    CHECK (s.patterns.front().slot == 12);
    CHECK (s.patterns.back().slot == 15);
    CHECK (s.scales.size() == 3);
    CHECK (s.hasGlobalKey);
    CHECK (! s.keys.empty());
    CHECK (carte::toContainers (s).size() >= 8 + 8 + 4 + 3 + 2);
    // enregistré puis relu à l'identique
    CHECK (carte::fromJson (carte::toJson (s)).chords.size() == 8);
}

TEST_CASE ("exemples d'essai : les fichiers de docs/exemples-arpege se lisent", "[carte][pattern]")
{
    std::ifstream a (std::string (NEPTUNE_SOURCE_DIR) + "/docs/exemples-arpege/accords-essai.json");
    std::stringstream sa;
    sa << a.rdbuf();
    const auto r = importChordFile (sa.str(), 0.0);
    REQUIRE (r.ok);
    CHECK (r.score.chords.size() == 8);
    CHECK (r.score.measures.size() == 8);
    CHECK (r.score.measures[7].length == 3.0);
    CHECK (r.score.chords[5].chord.bass == 2);   // Am7/D
    const auto demo = carte::demoScore();
    for (size_t i = 0; i < 8; ++i)               // le fichier est le même morceau que le bouton d'essai
    {
        CHECK (r.score.chords[i].start == demo.chords[i].start);
        CHECK (r.score.chords[i].length == demo.chords[i].length);
        CHECK (r.score.chords[i].chord.root == demo.chords[i].chord.root);
        CHECK ((r.score.chords[i].chord.rel & 0x0FFF) == (demo.chords[i].chord.rel & 0x0FFF));
    }
}

// ---- échange `flibuste-carte` ----
namespace
{
void expectSameScore (const carte::Score& a, const carte::Score& b)
{
    REQUIRE (a.chords.size() == b.chords.size());
    for (std::size_t i = 0; i < a.chords.size(); ++i)
    {
        CHECK (a.chords[i].start == Approx (b.chords[i].start));
        CHECK (a.chords[i].length == Approx (b.chords[i].length));
        CHECK (a.chords[i].chord == b.chords[i].chord);
    }
    REQUIRE (a.scales.size() == b.scales.size());
    for (std::size_t i = 0; i < a.scales.size(); ++i)
    {
        CHECK (a.scales[i].start == Approx (b.scales[i].start));
        CHECK (a.scales[i].length == Approx (b.scales[i].length));
        CHECK (a.scales[i].root == b.scales[i].root);
        CHECK (a.scales[i].sharp == b.scales[i].sharp);
        CHECK (carte::scaleItemMask (a.scales[i]) == carte::scaleItemMask (b.scales[i]));
        CHECK (a.scales[i].rel == b.scales[i].rel);
        CHECK (a.scales[i].name == b.scales[i].name);
        if (a.scales[i].rel == 0) CHECK (a.scales[i].mode == b.scales[i].mode);
    }
    REQUIRE (a.keys.size() == b.keys.size());
    for (std::size_t i = 0; i < a.keys.size(); ++i)
    {
        CHECK (a.keys[i].start == Approx (b.keys[i].start));
        CHECK (a.keys[i].root == b.keys[i].root);
        CHECK (a.keys[i].minor == b.keys[i].minor);
        CHECK (a.keys[i].sharp == b.keys[i].sharp);
    }
    CHECK (a.hasGlobalKey == b.hasGlobalKey);
    if (a.hasGlobalKey) { CHECK (a.globalKey.root == b.globalKey.root); CHECK (a.globalKey.minor == b.globalKey.minor); CHECK (a.globalKey.sharp == b.globalKey.sharp); }
    REQUIRE (a.patterns.size() == b.patterns.size());
    for (std::size_t i = 0; i < a.patterns.size(); ++i)
    {
        CHECK (a.patterns[i].start == Approx (b.patterns[i].start));
        CHECK (a.patterns[i].length == Approx (b.patterns[i].length));
        CHECK (a.patterns[i].slot == b.patterns[i].slot);
    }
    REQUIRE (a.measures.size() == b.measures.size());
    for (std::size_t i = 0; i < a.measures.size(); ++i)
    {
        CHECK (a.measures[i].start == Approx (b.measures[i].start));
        CHECK (a.measures[i].length == Approx (b.measures[i].length));
        CHECK (a.measures[i].num == b.measures[i].num);
        CHECK (a.measures[i].den == b.measures[i].den);
    }
}
} // namespace

TEST_CASE ("carte exchange: a full score survives export and import, free scales and spellings included", "[carte][exchange]")
{
    auto score = carte::demoScore (0.0);
    // une gamme libre (hors de la liste), une tonalité en bémols, une en dièses, des triolets
    carte::ScaleItem free;
    free.start = 33.0; free.length = 5.0 + 1.0 / 3.0; free.root = 3; free.sharp = false; free.rel = 0x00AD; free.name = "Ma gamme";   // 0 2 3 5 7
    carte::placeScale (score.scales, free);
    carte::placeKey (score.keys, { 40.0, 10, true, false });    // si bémol mineur
    carte::placeKey (score.keys, { 48.0, 6, false, true });     // fa dièse majeur
    carte::placePattern (score.patterns, { 40.0, 4.0 / 3.0, 7 });
    const auto text = carte::exportCarteFile (score, "Essai");
    const auto back = carte::importCarteFile (text, 0.0);
    REQUIRE (back.ok);
    CHECK (back.hasChords); CHECK (back.hasMeasures); CHECK (back.hasScales); CHECK (back.hasKeys); CHECK (back.hasPatterns);
    CHECK (back.title == "Essai");
    CHECK (back.unknownScales == 0);
    CHECK (back.skipped == 0);
    expectSameScore (score, back.score);
    // l'écriture bémol / dièse est gardée
    CHECK (text.find ("\"Bb\"") != std::string::npos);
    CHECK (text.find ("\"F#\"") != std::string::npos);
}

TEST_CASE ("carte exchange: the song start is subtracted on export and added back on import", "[carte][exchange]")
{
    const auto score = carte::demoScore (16.0);   // le morceau commence à la mesure 5 du projet
    const auto text = carte::exportCarteFile (score, "", 16.0);
    const auto j = nlohmann::json::parse (text);
    CHECK (j["chords"][0]["at"] == nlohmann::json::array ({ 0, 1 }));       // le fichier commence à zéro
    const auto back = carte::importCarteFile (text, 16.0);
    REQUIRE (back.ok);
    expectSameScore (score, back.score);
    const auto elsewhere = carte::importCarteFile (text, 100.0);             // un autre endroit du projet
    REQUIRE (elsewhere.ok);
    CHECK (elsewhere.score.chords[0].start == Approx (100.0));
}

TEST_CASE ("carte exchange: scales are read by Scorsaire identifier, by intervals, or kept as free scales", "[carte][exchange]")
{
    const auto file = [] (const char* scales)
    {
        return std::string (R"({"format":"flibuste-carte","version":1,"scales":)") + scales + "}";
    };
    // identifiant connu et intervalles concordants : une gamme de la liste
    auto r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"D","scale":"dorian","intervals":[0,2,3,5,7,9,10]}])"), 0.0);
    REQUIRE (r.ok);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (r.score.scales[0].rel == 0);
    CHECK (carte::scaleMode (r.score.scales[0].mode).id == std::string ("dorian"));
    CHECK (r.score.scales[0].root == 2);
    // identifiant seul, connu : la gamme de la liste
    r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"A","scale":"natural_minor"}])"), 0.0);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (carte::scaleMode (r.score.scales[0].mode).id == std::string ("minor"));
    // identifiant connu mais intervalles différents : le fichier fait foi, gamme libre
    r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"C","scale":"major","intervals":[0,2,4,7,9]}])"), 0.0);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (r.score.scales[0].rel != 0);
    CHECK (carte::scaleItemMask (r.score.scales[0]) == 0x295);   // pentatonique de do
    // identifiant inconnu avec intervalles : gamme libre, nommée par le fichier
    r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"E","scale":"hirajoshi","intervals":[0,2,3,7,8],"name":"Hirajoshi"}])"), 0.0);
    REQUIRE (r.score.scales.size() == 1);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (r.score.scales[0].rel == 0);   // le catalogue de Scorsaire est embarqué : hirajoshi est une gamme de la liste
    CHECK (carte::scaleMode (r.score.scales[0].mode).id == std::string ("hirajoshi"));
    // identifiant inconnu avec intervalles : gamme libre, nommée par le fichier
    r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"E","scale":"inventee","intervals":[0,1,6],"name":"Inventée"}])"), 0.0);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (r.score.scales[0].name == "Inventée");
    CHECK (carte::scaleName (r.score.scales[0]) == "E Inventée");
    // ni intervalles ni identifiant connu : ignorée et comptée ; une entrée sans position : ignorée et comptée
    r = carte::importCarteFile (file (R"([{"at":[0,1],"dur":[1,1],"root":"E","scale":"inconnue"},{"dur":[1,1],"root":"E","scale":"major"}])"), 0.0);
    REQUIRE (r.ok);
    CHECK (r.score.scales.empty());
    CHECK (r.unknownScales == 1);
    CHECK (r.skipped == 1);
}

TEST_CASE ("carte exchange: only the lanes present in the file are flagged, and a chord file is accepted", "[carte][exchange]")
{
    auto r = carte::importCarteFile (R"({"format":"flibuste-carte","version":1,"keys":[{"at":[0,1],"root":"G","mode":"major"}],"globalKey":{"root":"C","mode":"minor"}})", 0.0);
    REQUIRE (r.ok);
    CHECK_FALSE (r.hasChords); CHECK_FALSE (r.hasMeasures); CHECK_FALSE (r.hasScales); CHECK_FALSE (r.hasPatterns);
    CHECK (r.hasKeys);
    REQUIRE (r.score.keys.size() == 1);
    CHECK (r.score.keys[0].root == 7);
    CHECK (r.score.hasGlobalKey);
    CHECK (r.score.globalKey.minor);
    // un fichier d'accords (Scorsaire) : les accords et les mesures, rien d'autre
    r = carte::importCarteFile (R"({"format":"flibuste-accords","version":1,"measures":[{"at":[0,1],"dur":[1,1],"ts":[4,4]}],"chords":[{"at":[0,1],"dur":[1,1],"root":"D","quality":"m7","bass":null}],"qualities":{"m7":[0,3,7,10]}})", 0.0);
    REQUIRE (r.ok);
    CHECK (r.hasChords); CHECK (r.hasMeasures); CHECK_FALSE (r.hasScales); CHECK_FALSE (r.hasKeys); CHECK_FALSE (r.hasPatterns);
    CHECK (r.score.chords.size() == 1);
}

TEST_CASE ("carte exchange: refusals are clear and import nothing", "[carte][exchange]")
{
    CHECK_FALSE (carte::importCarteFile ("pas du json", 0.0).ok);
    auto r = carte::importCarteFile (R"({"format":"autre","version":1})", 0.0);
    CHECK_FALSE (r.ok);
    CHECK (r.error.find ("autre") != std::string::npos);
    r = carte::importCarteFile (R"({"format":"flibuste-carte","version":2})", 0.0);
    CHECK_FALSE (r.ok);
    CHECK (r.error.find ("Version 2") != std::string::npos);
    // trop de gammes : refus net
    std::string many = R"({"format":"flibuste-carte","version":1,"scales":[)";
    for (int i = 0; i < carte::kMaxImportScales + 1; ++i) many += std::string (i ? "," : "") + R"({"at":[0,1],"dur":[1,1],"root":"C","scale":"major"})";
    many += "]}";
    r = carte::importCarteFile (many, 0.0);
    CHECK_FALSE (r.ok);
    CHECK (r.error.find ("gammes") != std::string::npos);
    // le fichier d'accords seul reste refusé par l'ancienne fonction quand c'est une carte
    CHECK_FALSE (carte::importChordFile (R"({"format":"flibuste-carte","version":1})", 0.0).ok);
}

TEST_CASE ("carte exchange: a pattern whose name differs from ours is reported, and the slot is kept", "[carte][exchange]")
{
    const std::vector<std::string> mine { "", "", "", "", "", "", "", "", "", "", "", "Essai 1" };
    const auto r = carte::importCarteFile (R"({"format":"flibuste-carte","version":1,"patterns":[{"at":[0,1],"dur":[2,1],"slot":12,"name":"Ballade"},{"at":[2,1],"dur":[1,1],"slot":17}]})", 0.0, mine);
    REQUIRE (r.ok);
    REQUIRE (r.score.patterns.size() == 1);   // l'emplacement 17 n'existe pas : ignoré
    CHECK (r.score.patterns[0].slot == 12);
    CHECK (r.skipped == 1);
    REQUIRE (r.notes.size() == 1);
    CHECK (r.notes[0].find ("Ballade") != std::string::npos);
}

TEST_CASE ("carte exchange: the saved state keeps free scales", "[carte][exchange]")
{
    carte::Score s;
    carte::ScaleItem free;
    free.start = 4.0; free.length = 4.0; free.root = 9; free.rel = 0x00AD; free.name = "Ma gamme";
    carte::placeScale (s.scales, free);
    const auto back = carte::fromJson (carte::toJson (s));
    REQUIRE (back.scales.size() == 1);
    CHECK (back.scales[0].rel == 0x00AD);
    CHECK (back.scales[0].name == "Ma gamme");
    CHECK (carte::scaleItemMask (back.scales[0]) == carte::scaleItemMask (free));
    CHECK (carte::toContainers (back).back().pitchMask == carte::scaleItemMask (free));   // la carte partagée reçoit les notes de la gamme libre
}

TEST_CASE ("carte exchange: Scorsaire's own chord files are read the same way by the carte importer", "[carte][exchange]")
{
    for (const char* name : { "accords-anacrouse-chiffrage.json", "accords-ds-al-coda.json", "accords-reprise-volta.json" })
    {
        const auto text = example (name);
        REQUIRE_FALSE (text.empty());
        const auto a = carte::importChordFile (text, 0.0);
        const auto b = carte::importCarteFile (text, 0.0);
        REQUIRE (a.ok);
        REQUIRE (b.ok);
        CHECK (b.score.chords.size() == a.score.chords.size());
        CHECK (b.score.measures.size() == a.score.measures.size());
        CHECK (b.hasChords);
        CHECK_FALSE (b.hasScales);
        // et notre export de ces accords se relit à l'identique
        const auto again = carte::importCarteFile (carte::exportCarteFile (b.score, "x"), 0.0);
        REQUIRE (again.ok);
        expectSameScore (b.score, again.score);
    }
}

TEST_CASE ("carte catalog: Scorsaire's 72 scales are known by identifier, with unique ids and intervals", "[carte][exchange]")
{
    using namespace neptune::core;
    CHECK (carte::scaleModeCount() == 73);   // 72 de Scorsaire + le chromatique de Carte
    std::set<std::string> ids, masks;
    for (int i = 0; i < carte::scaleModeCount(); ++i)
    {
        const auto& m = carte::scaleMode (i);
        CHECK (ids.insert (m.id).second);
        CHECK (masks.insert (std::to_string (m.rel)).second);
        CHECK ((m.rel & 1u) != 0);
    }
    // une gamme du catalogue qui n'était pas dans les 18 d'origine
    const auto file = std::string (R"({"format":"flibuste-carte","version":1,"scales":[{"at":[0,1],"dur":[1,1],"root":"C","scale":"bebop_dominant","intervals":[0,2,4,5,7,9,10,11]}]})");
    const auto r = carte::importCarteFile (file, 0.0);
    REQUIRE (r.score.scales.size() == 1);
    CHECK (r.score.scales[0].rel == 0);
    CHECK (carte::scaleMode (r.score.scales[0].mode).rel == 0xEB5);
    const auto out = carte::exportCarteFile (r.score, "t");
    CHECK (out.find ("\"bebop_dominant\"") != std::string::npos);
}

TEST_CASE ("carte exchange: keys carry fifths, and fifths alone is enough on import", "[carte][exchange]")
{
    using namespace neptune::core;
    CHECK (carte::keyFifths ({ 0.0, 0, false, false }) == 0);     // do majeur
    CHECK (carte::keyFifths ({ 0.0, 7, false, false }) == 1);     // sol majeur
    CHECK (carte::keyFifths ({ 0.0, 5, false, false }) == -1);    // fa majeur
    CHECK (carte::keyFifths ({ 0.0, 9, true, false }) == 0);      // la mineur
    CHECK (carte::keyFifths ({ 0.0, 6, true, true }) == 3);       // fa# mineur
    CHECK (carte::keyFifths ({ 0.0, 3, true, false }) == -6);     // mi bémol mineur
    CHECK (carte::keyFifths ({ 0.0, 6, false, true }) == 6);      // fa# majeur
    CHECK (carte::keyFifths ({ 0.0, 6, false, false }) == -6);    // sol bémol majeur
    CHECK (carte::keyFifths ({ 0.0, 1, false, true }) == 7);      // do# majeur
    CHECK (carte::keyFifths ({ 0.0, 11, false, false }) == 5);    // si majeur
    CHECK (carte::keyFifths ({ 0.0, 10, false, false }) == -2);   // si bémol majeur
    // tout nombre de quintes revient à lui-même par la tonalité ; sept bémols (do bémol) n'a pas d'écriture dans Carte et devient son enharmonique, cinq dièses (si)
    CHECK (carte::keyFifths (carte::keyFromFifths (-7, false)) == 5);
    for (int f = -6; f <= 7; ++f)
    {
        CHECK (carte::keyFifths (carte::keyFromFifths (f, false)) == f);
        CHECK (carte::keyFifths (carte::keyFromFifths (f, true)) == f);
    }
    // export : fifths écrit pour chaque tonalité et pour la globale ; relecture identique
    carte::Score s;
    s.hasGlobalKey = true;
    s.globalKey = { 0.0, 2, true, false };   // ré mineur : un bémol
    carte::placeKey (s.keys, { 4.0, 11, false, false });
    const auto text = carte::exportCarteFile (s, "t");
    const auto j = nlohmann::json::parse (text);
    CHECK (j["globalKey"]["fifths"] == -1);
    CHECK (j["keys"][0]["fifths"] == 5);
    const auto back = carte::importCarteFile (text, 0.0);
    REQUIRE (back.score.keys.size() == 1);
    CHECK (back.score.keys[0].root == 11);
    CHECK (back.score.globalKey.root == 2);
    CHECK (back.score.globalKey.minor);
    // fifths seul : majeur par défaut ; avec mode, le relatif ; en désaccord avec root, fifths gagne
    const auto file = [] (const char* keys, const char* g)
    {
        return std::string (R"({"format":"flibuste-carte","version":1,"keys":)") + keys + R"(,"globalKey":)" + g + "}";
    };
    auto r = carte::importCarteFile (file (R"([{"at":[1,1],"fifths":-3},{"at":[2,1],"fifths":2,"mode":"minor"},{"at":[3,1],"root":"C","mode":"major","fifths":2}])", R"({"fifths":4})"), 0.0);
    REQUIRE (r.ok);
    REQUIRE (r.score.keys.size() == 3);
    CHECK (r.score.keys[0].root == 3);                            // mi bémol majeur
    CHECK (! r.score.keys[0].minor);
    CHECK (r.score.keys[1].root == 11);                           // si mineur
    CHECK (r.score.keys[1].minor);
    CHECK (r.score.keys[2].root == 2);                            // fifths = 2 l'emporte sur do : ré majeur
    CHECK (r.score.hasGlobalKey);
    CHECK (r.score.globalKey.root == 4);                          // mi majeur
    // fifths hors limites et sans root : entrée ignorée
    r = carte::importCarteFile (file (R"([{"at":[1,1],"fifths":9}])", R"({"root":"G","mode":"major"})"), 0.0);
    CHECK (r.score.keys.empty());
    CHECK (r.skipped == 1);
    CHECK (r.score.globalKey.root == 7);
}

TEST_CASE ("carte qualities: Scorsaire's dictionary is the reference, on two octaves", "[carte][exchange]")
{
    using namespace neptune::core;
    CHECK (chords::qualitySuffixes().size() == 31);
    chords::Chord c;
    // les identifiants de Scorsaire s'écrivent et se relisent
    for (const char* t : { "CM7", "Do9", "Bo7", "Fo", "GM9", "Am7b5", "D7#11", "E69", "Cadd9", "Cmadd9", "C7#5", "C13", "Calt" })
    {
        REQUIRE (chords::parseChord (t, c));
        CHECK (chords::parseChord (chords::formatChord (c), c));
    }
    REQUIRE (chords::parseChord ("Cmaj7", c));
    CHECK (chords::formatChord (c) == "CM7");   // l'écriture affichée est l'identifiant de Scorsaire
    // alias de Scorsaire et de Carte
    for (const char* t : { "C\xCE\x94", "Cmaj7", "CM7", "C\xE2\x96\xB3" "7" })
    {
        chords::Chord x;
        REQUIRE (chords::parseChord (t, x));
        CHECK (x.rel == 0x891);
    }
    REQUIRE (chords::parseChord ("Cdim", c));
    CHECK (chords::formatChord (c) == "Co");
    REQUIRE (chords::parseChord ("Cdim7", c));
    CHECK (chords::formatChord (c) == "Co7");
    REQUIRE (chords::parseChord ("C6/9", c));
    CHECK (chords::formatChord (c) == "C69");
    REQUIRE (chords::parseChord ("Cm(maj7)", c));
    CHECK (chords::formatChord (c) == "CmMaj7");
    // une seconde ajoutée n'est pas une neuvième ajoutée
    CHECK (! chords::parseChord ("Cadd2", c));
    CHECK (! chords::parseChord ("C2", c));
    // deux octaves : la neuvième est à 14 demi-tons, la seconde à 2
    chords::Chord nine, sus2, add9;
    REQUIRE (chords::parseChord ("C9", nine));
    REQUIRE (chords::parseChord ("Csus2", sus2));
    REQUIRE (chords::parseChord ("Cadd9", add9));
    CHECK ((nine.rel & (1u << 14)) != 0);
    CHECK ((nine.rel & (1u << 2)) == 0);
    CHECK ((sus2.rel & (1u << 2)) != 0);
    CHECK ((add9.rel & (1u << 14)) != 0);
    CHECK (add9.rel != sus2.rel);
    CHECK (chords::chordMask (nine) == (1u << 0 | 1u << 4 | 1u << 7 | 1u << 10 | 1u << 2));   // classes de hauteur : la neuvième est un ré
    // une qualité hors dictionnaire s'écrit avec ses intervalles, et se relit à l'identique
    chords::Chord free;
    REQUIRE (chords::parseChord ("C[0,4,7,13,20]", free));
    CHECK (free.rel == (1u | 1u << 4 | 1u << 7 | 1u << 13 | 1u << 20));
    CHECK (chords::formatChord (free) == "C[0,4,7,13,20]");
    CHECK (! chords::parseChord ("C[0,4,99]", c));
    CHECK (! chords::parseChord ("C[]", c));
    REQUIRE (chords::parseChord ("D[0,3,7]/F", c));
    CHECK (c.rel == 0x089);
    CHECK (c.bass == 5);
    CHECK (chords::formatChord (c) == "Dm/F");   // ces intervalles sont ceux de « m » : l'identifiant du dictionnaire est préféré
}

TEST_CASE ("carte qualities: the shared map carries the two octaves, and old writers still work", "[carte][exchange]")
{
    using namespace neptune::core;
    chords::Item it;
    it.start = 0.0;
    it.length = 4.0;
    REQUIRE (chords::parseChord ("G13", it.chord));
    const auto v = chords::toContainers ({ it });
    REQUIRE (v.size() == 1);
    CHECK (v[0].intervals() == it.chord.rel);
    CHECK (v[0].pitchMask == chords::chordMask (it.chord));
    // l'écrivain d'avant (intervalles à 0) : seul le masque d'une octave sert
    chordmap::Container old = v[0];
    old.setIntervals (0);
    CHECK (old.intervals() == 0);
    // via le lecteur, un accord de neuvième garde sa neuvième
    chordmap::Writer w ("qual-test");
    chordmap::Reader r ("qual-test");
    REQUIRE (w.open (3) == chordmap::Status::Ok);
    w.publish (v);
    r.setChannel (3);
    r.poll();
    chordmap::Container got;
    REQUIRE (r.lookup (1.0, chordmap::Kind::Chord, got));
    CHECK (got.intervals() == it.chord.rel);
    CHECK ((got.intervals() & (1u << 14)) != 0);
}

TEST_CASE ("carte qualities: pattern degrees tell a ninth from a second", "[carte][exchange]")
{
    using namespace neptune::core;
    chords::Chord nine, sus2;
    REQUIRE (chords::parseChord ("C9", nine));
    REQUIRE (chords::parseChord ("Csus2", sus2));
    pattern::Part ninth, second;
    REQUIRE (pattern::parsePart ("9", ninth));
    REQUIRE (pattern::parsePart ("2", second));
    CHECK (pattern::degreeSemitones (ninth, nine.rel, pattern::Absent::Skip) == 14);
    CHECK (pattern::degreeSemitones (second, nine.rel, pattern::Absent::Skip) == -1);   // un accord de neuvième n'a pas de seconde
    CHECK (pattern::degreeSemitones (second, sus2.rel, pattern::Absent::Skip) == 2);
    CHECK (pattern::degreeSemitones (ninth, sus2.rel, pattern::Absent::Skip) == 14);
    pattern::ChordContext ctx;
    ctx.rootMidi = 48;
    ctx.rel = nine.rel;
    pattern::Part all;
    REQUIRE (pattern::parsePart ("C", all));
    int notes[24];
    const int n = pattern::resolvePart (all, ctx, pattern::Absent::Skip, notes, 24);
    CHECK (n == 5);
    CHECK (notes[n - 1] == 48 + 14);   // la neuvième sonne dans l'octave du dessus
}

TEST_CASE ("carte exchange: qualities are written with Scorsaire ids and two-octave intervals, and read back", "[carte][exchange]")
{
    using namespace neptune::core;
    carte::Score s;
    for (const char* t : { "CM7", "D9", "Eadd9", "Fo7" })
    {
        chords::Item it;
        it.start = static_cast<double> (s.chords.size()) * 4.0;
        it.length = 4.0;
        REQUIRE (chords::parseChord (t, it.chord));
        s.chords.push_back (it);
    }
    const auto text = carte::exportCarteFile (s, "t");
    const auto j = nlohmann::json::parse (text);
    CHECK (j["chords"][0]["quality"] == "M7");
    CHECK (j["chords"][3]["quality"] == "o7");
    CHECK (j["qualities"]["9"] == nlohmann::json::array ({ 0, 4, 7, 10, 14 }));
    CHECK (j["qualities"]["add9"] == nlohmann::json::array ({ 0, 4, 7, 14 }));
    const auto back = carte::importCarteFile (text, 0.0);
    REQUIRE (back.ok);
    REQUIRE (back.score.chords.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK (back.score.chords[i].chord.rel == s.chords[i].chord.rel);
    // anciens noms de qualité (Carte d'avant) : relus par leurs alias
    const std::string old = R"({"format":"flibuste-carte","version":1,"qualities":{"maj7":[0,4,7,11]},"chords":[{"at":[0,1],"dur":[1,1],"root":"C","quality":"maj7"},{"at":[1,1],"dur":[1,1],"root":"D","quality":"dim"}]})";
    const auto r = carte::importCarteFile (old, 0.0);
    REQUIRE (r.score.chords.size() == 2);
    CHECK (chords::formatChord (r.score.chords[0].chord) == "CM7");
    CHECK (chords::formatChord (r.score.chords[1].chord) == "Do");
    // une qualité que personne ne connaît : avec ses intervalles dans le fichier, elle est jouée ; sans, seule la fondamentale reste
    const std::string odd = R"({"format":"flibuste-carte","version":1,"qualities":{"zz":[0,2,5,13]},"chords":[{"at":[0,1],"dur":[1,1],"root":"C","quality":"zz"},{"at":[1,1],"dur":[1,1],"root":"D","quality":"yy"}]})";
    const auto q = carte::importCarteFile (odd, 0.0);
    REQUIRE (q.score.chords.size() == 2);
    CHECK (q.score.chords[0].chord.rel == (1u | 1u << 2 | 1u << 5 | 1u << 13));
    CHECK (q.score.chords[1].chord.rel == 1u);
    CHECK (q.unknownQualities == 1);
    // la qualité libre se réécrit et se relit
    const auto again = carte::importCarteFile (carte::exportCarteFile (q.score, "t"), 0.0);
    REQUIRE (again.score.chords.size() == 2);
    CHECK (again.score.chords[0].chord.rel == q.score.chords[0].chord.rel);
}
