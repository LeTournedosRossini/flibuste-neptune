// Timeline d'accords : noms, édition sans chevauchement, enregistrement en texte, conversion vers la carte partagée.
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "neptune/core/ChordTimeline.h"

using namespace neptune::core::chords;

namespace
{
Chord parsed (const char* text)
{
    Chord c;
    REQUIRE (parseChord (text, c));
    return c;
}
Item item (double start, double length, const char* chord)
{
    Item it;
    it.start = start;
    it.length = length;
    it.chord = parsed (chord);
    return it;
}
} // namespace

TEST_CASE ("accords : lecture des noms usuels", "[chords]")
{
    CHECK (parsed ("C").root == 0);
    CHECK (parsed ("C").rel == 0x091);
    CHECK (parsed ("Dm7").root == 2);
    CHECK (parsed ("Dm7").rel == ((1u << 0) | (1u << 3) | (1u << 7) | (1u << 10)));
    CHECK (parsed ("G7").rel == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 10)));
    CHECK (parsed ("Cmaj7").rel == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 11)));
    CHECK (parsed ("CM7").rel == parsed ("Cmaj7").rel);
    CHECK (parsed ("Cm").rel == ((1u << 0) | (1u << 3) | (1u << 7)));
    CHECK (parsed ("Bb").root == 10);
    CHECK (parsed ("F#m7b5").root == 6);
    CHECK (parsed ("F#m7b5").sharp);
    CHECK (parsed ("Ebdim").rel == ((1u << 0) | (1u << 3) | (1u << 6)));
    CHECK (parsed ("Csus4").rel == ((1u << 0) | (1u << 5) | (1u << 7)));
    CHECK (parsed ("c").root == 0);
    CHECK (parsed ("am").root == 9);
    CHECK (parsed ("am").rel == parsed ("Am").rel);
}

TEST_CASE ("accords : noms français et basse", "[chords]")
{
    CHECK (parsed ("Do").root == 0);
    CHECK (parsed ("R\xC3\xA9m7").root == 2);
    CHECK (parsed ("Re").root == 2);
    CHECK (parsed ("Sol7").root == 7);
    CHECK (parsed ("Sib").root == 10);
    CHECK (parsed ("Lam").root == 9);
    CHECK (parsed ("Lam").rel == parsed ("Am").rel);
    CHECK (parsed ("Mib").root == 3);
    const auto slash = parsed ("C/E");
    CHECK (slash.root == 0);
    CHECK (slash.bass == 4);
    CHECK (parsed ("C/C").bass == -1);
    CHECK (parsed ("Do/Sol").bass == 7);
}

TEST_CASE ("accords : textes refusés", "[chords]")
{
    Chord c;
    CHECK_FALSE (parseChord ("", c));
    CHECK_FALSE (parseChord ("   ", c));
    CHECK_FALSE (parseChord ("H", c));
    CHECK_FALSE (parseChord ("Cxyz", c));
    CHECK_FALSE (parseChord ("C/", c));
    CHECK_FALSE (parseChord ("C/Z", c));
    CHECK_FALSE (parseChord ("C/E7", c));
}

TEST_CASE ("accords : écriture relue à l'identique", "[chords]")
{
    for (const char* t : { "C", "Dm7", "F#m7b5", "Bb", "G7", "Cmaj7", "Ebdim7", "Asus2", "C/E", "Am/G", "F#7/A#", "Dbmaj9", "E7#9" })
    {
        const auto c = parsed (t);
        Chord again;
        REQUIRE (parseChord (formatChord (c), again));
        CHECK (again == c);
    }
    CHECK (formatChord (parsed ("Bb")) == "Bb");
    CHECK (formatChord (parsed ("A#")) == "A#");
    CHECK (formatChord (parsed ("Dm7"), Naming::French) == "R\xC3\xA9m7");
    CHECK (formatChord (parsed ("C/E"), Naming::French) == "Do/Mi");
}

TEST_CASE ("accords : masque absolu avec basse", "[chords]")
{
    CHECK (absoluteMask (parsed ("C")) == ((1u << 0) | (1u << 4) | (1u << 7)));
    CHECK (absoluteMask (parsed ("Am")) == ((1u << 9) | (1u << 0) | (1u << 4)));
    CHECK (absoluteMask (parsed ("C/D")) == ((1u << 0) | (1u << 4) | (1u << 7) | (1u << 2)));
}

TEST_CASE ("timeline : poser coupe ou retire ce qui recouvre", "[chords]")
{
    std::vector<Item> v;
    CHECK (place (v, item (0, 4, "C")) == 0);
    CHECK (place (v, item (4, 4, "F")) == 1);
    CHECK (place (v, item (8, 4, "G")) == 2);

    // posé au milieu de C : C raccourci, tout ce qui suit intact
    place (v, item (2, 4, "Am"));   // recouvre la fin de C et le début de F
    REQUIRE (v.size() == 4);
    CHECK (v[0].start == 0.0); CHECK (v[0].length == 2.0);
    CHECK (v[1].start == 2.0); CHECK (v[1].length == 4.0);
    CHECK (v[2].start == 6.0); CHECK (v[2].length == 2.0);   // F coupé au début
    CHECK (v[3].start == 8.0);

    // posé sur plusieurs : les couverts disparaissent
    place (v, item (0, 12, "Dm"));
    REQUIRE (v.size() == 1);
    CHECK (formatChord (v[0].chord) == "Dm");

    // au milieu d'un seul : il est coupé en deux
    place (v, item (4, 2, "G"));
    REQUIRE (v.size() == 3);
    CHECK (v[0].length == 4.0);
    CHECK (v[2].start == 6.0);
    CHECK (v[2].length == 6.0);
    CHECK (formatChord (v[2].chord) == "Dm");
}

TEST_CASE ("timeline : déplacer, redimensionner, retirer", "[chords]")
{
    std::vector<Item> v;
    place (v, item (0, 4, "C"));
    place (v, item (4, 4, "F"));
    place (v, item (8, 4, "G"));

    const int i = moveItem (v, 0, 8.0);   // C posé sur G
    CHECK (i >= 0);
    REQUIRE (v.size() == 2);
    CHECK (formatChord (v[1].chord) == "C");
    CHECK (v[1].start == 8.0);

    CHECK (resizeItem (v, 0, 2.0) == 0);   // F raccourci
    CHECK (v[0].length == 2.0);
    CHECK (resizeItem (v, 0, 100.0) == 0);
    CHECK (v.size() == 1);                 // F allongé a couvert C

    CHECK (resizeItem (v, 0, 0.0) == 0);   // jamais plus court que le minimum
    CHECK (v[0].length == kMinLength);

    CHECK_FALSE (removeItem (v, 5));
    CHECK (removeItem (v, 0));
    CHECK (v.empty());
    CHECK (moveItem (v, 0, 1.0) == -1);
}

TEST_CASE ("timeline : valeurs invalides refusées", "[chords]")
{
    std::vector<Item> v;
    CHECK (place (v, item (-1, 4, "C")) == -1);
    CHECK (place (v, item (0, 0, "C")) == -1);
    CHECK (place (v, item (0, std::numeric_limits<double>::quiet_NaN(), "C")) == -1);
    CHECK (v.empty());
}

TEST_CASE ("timeline : indexAt", "[chords]")
{
    std::vector<Item> v;
    place (v, item (0, 4, "C"));
    place (v, item (8, 4, "G"));
    CHECK (indexAt (v, 0.0) == 0);
    CHECK (indexAt (v, 3.99) == 0);
    CHECK (indexAt (v, 4.0) == -1);
    CHECK (indexAt (v, 8.0) == 1);
    CHECK (indexAt (v, 12.0) == -1);
}

TEST_CASE ("timeline : texte d'enregistrement", "[chords]")
{
    std::vector<Item> v;
    place (v, item (0, 4, "C"));
    place (v, item (4.5, 1.25, "F#m7b5/C"));
    place (v, item (8, 4, "Bb"));
    const auto text = toText (v);
    const auto back = fromText (text);
    REQUIRE (back.size() == v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        CHECK (back[i].start == v[i].start);
        CHECK (back[i].length == v[i].length);
        CHECK (back[i].chord == v[i].chord);
    }
    // texte abîmé : lignes invalides ignorées, chevauchements réglés
    const auto bad = fromText ("0 4 C\nn'importe quoi\n2 4 Xyz\n2 4 G\n-3 4 D\n\n");
    REQUIRE (bad.size() == 2);
    CHECK (bad[0].length == 2.0);
    CHECK (formatChord (bad[1].chord) == "G");
    CHECK (fromText ("").empty());
}

TEST_CASE ("timeline : conteneurs publiés", "[chords]")
{
    std::vector<Item> v;
    place (v, item (0, 4, "C/E"));
    const auto c = toContainers (v);
    REQUIRE (c.size() == 1);
    CHECK (c[0].startBeats == 0.0);
    CHECK (c[0].lengthBeats == 4.0);
    CHECK (c[0].root == 0);
    CHECK (c[0].bass == 4);
    CHECK (c[0].pitchMask == absoluteMask (v[0].chord));
    place (v, item (4, 4, "G"));
    CHECK (toContainers (v)[1].bass == 255);
}
