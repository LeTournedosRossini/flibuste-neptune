// Carte v2 : gammes, tonalités, motifs, mesures, enregistrement, publication dans la carte partagée, import des fichiers de Scorsaire.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "neptune/core/CarteScore.h"

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
    CHECK (r.score.scales[0].name == "Hirajoshi");
    CHECK (carte::scaleName (r.score.scales[0]) == "E Hirajoshi");
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
