// Arpégiateur : motifs, instants à l'échantillon près, blocs de toute taille, arrêt / saut / boucle sans note bloquée.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "neptune/core/Arpeggiator.h"

using namespace neptune::core::arp;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kBpm = 120.0;                       // 24000 échantillons par noire
constexpr double kSamplesPerBeat = kSr * 60.0 / kBpm;

struct Event { long long at; bool on; int note; int vel; };

// Joue `totalSamples` en blocs de `block` échantillons ; `notes` = événements d'entrée avec leur instant absolu.
std::vector<Event> run (Arpeggiator& a, const std::vector<Event>& notes, long long totalSamples, int block, bool playing = true, double ppq0 = 0.0)
{
    std::vector<Event> events;
    std::vector<NoteOut> out (4096);
    long long pos = 0;
    while (pos < totalSamples)
    {
        const int n = (int) std::min<long long> (block, totalSamples - pos);
        std::vector<NoteIn> in;
        for (const auto& e : notes)
            if (e.at >= pos && e.at < pos + n) in.push_back ({ (int) (e.at - pos), e.on, (std::uint8_t) e.note, (std::uint8_t) e.vel });
        Transport tr;
        tr.playing = playing;
        tr.hasPosition = playing;
        tr.ppq = ppq0 + (double) pos / kSamplesPerBeat;
        tr.bpm = kBpm;
        const int m = a.process (in.data(), (int) in.size(), tr, n, out.data(), (int) out.size());
        for (int i = 0; i < m; ++i) events.push_back ({ pos + out[i].offset, out[i].on, out[i].note, out[i].velocity });
        pos += n;
    }
    return events;
}

std::vector<int> onNotes (const std::vector<Event>& ev)
{
    std::vector<int> v;
    for (const auto& e : ev)
        if (e.on) v.push_back (e.note);
    return v;
}

Arpeggiator make (Pattern p = Pattern::Up, double step = 0.25)
{
    Arpeggiator a;
    a.setSampleRate (kSr);
    Params prm;
    prm.pattern = p;
    prm.stepBeats = step;
    a.setParams (prm);
    return a;
}

const std::vector<Event> kChord = { { 0, true, 60, 90 }, { 0, true, 64, 90 }, { 0, true, 67, 90 } };
} // namespace

TEST_CASE ("arp : montant sur un accord, instants exacts", "[arp]")
{
    auto a = make();
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 2), 512);
    const auto ons = onNotes (ev);
    REQUIRE (ons.size() == 8);
    const std::vector<int> expect = { 60, 64, 67, 60, 64, 67, 60, 64 };
    CHECK (ons == expect);
    int k = 0;
    for (const auto& e : ev)
        if (e.on) CHECK (e.at == (long long) std::llround (k++ * 0.25 * kSamplesPerBeat));
}

TEST_CASE ("arp : le résultat ne dépend pas de la taille des blocs", "[arp]")
{
    std::vector<Event> ref;
    for (int block : { 1, 7, 64, 256, 1000, 4096 })
    {
        auto a = make (Pattern::UpDown);
        const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 4), block);
        if (ref.empty()) ref = ev;
        REQUIRE (ev.size() == ref.size());
        for (size_t i = 0; i < ev.size(); ++i)
        {
            CHECK (ev[i].at == ref[i].at);
            CHECK (ev[i].on == ref[i].on);
            CHECK (ev[i].note == ref[i].note);
        }
    }
}

TEST_CASE ("arp : motifs", "[arp]")
{
    const long long len = (long long) (kSamplesPerBeat * 2);   // 8 pas
    SECTION ("descendant")
    {
        auto a = make (Pattern::Down);
        CHECK (onNotes (run (a, kChord, len, 256)) == std::vector<int> { 67, 64, 60, 67, 64, 60, 67, 64 });
    }
    SECTION ("aller-retour sans répéter les bouts")
    {
        auto a = make (Pattern::UpDown);
        CHECK (onNotes (run (a, kChord, len, 256)) == std::vector<int> { 60, 64, 67, 64, 60, 64, 67, 64 });
    }
    SECTION ("aller-retour en répétant les bouts")
    {
        auto a = make (Pattern::UpDownRepeat);
        CHECK (onNotes (run (a, kChord, len, 256)) == std::vector<int> { 60, 64, 67, 67, 64, 60, 60, 64 });
    }
    SECTION ("ordre de saisie")
    {
        auto a = make (Pattern::AsPlayed);
        const std::vector<Event> in = { { 0, true, 67, 90 }, { 0, true, 60, 90 }, { 0, true, 64, 90 } };
        CHECK (onNotes (run (a, in, len, 256)) == std::vector<int> { 67, 60, 64, 67, 60, 64, 67, 60 });
    }
    SECTION ("accord : toutes les notes ensemble")
    {
        auto a = make (Pattern::Chord);
        const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 0.25 * 2), 256);
        int ons = 0;
        for (const auto& e : ev) if (e.on) ++ons;
        CHECK (ons == 6);
        CHECK (ev[0].at == 0);
        CHECK (ev[1].at == 0);
        CHECK (ev[2].at == 0);
    }
    SECTION ("deux octaves")
    {
        auto a = make();
        Params p = a.params();
        p.octaves = 2;
        a.setParams (p);
        CHECK (onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 1.5), 256)) == std::vector<int> { 60, 64, 67, 72, 76, 79 });
    }
    SECTION ("aléatoire : seulement des notes de l'accord")
    {
        auto a = make (Pattern::Random);
        for (int n : onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 8), 256))) CHECK ((n == 60 || n == 64 || n == 67));
    }
}

TEST_CASE ("arp : durée de note, fins programmées, même à cheval sur deux blocs", "[arp]")
{
    auto a = make();
    Params p = a.params();
    p.gate = 0.5;
    a.setParams (p);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 2), 100);
    const long long stepLen = (long long) (0.25 * kSamplesPerBeat);
    int checked = 0;
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].on)
            for (size_t j = i + 1; j < ev.size(); ++j)
                if (! ev[j].on && ev[j].note == ev[i].note)
                {
                    CHECK (ev[j].at - ev[i].at == stepLen / 2);
                    ++checked;
                    break;
                }
    CHECK (checked >= 7);
}

TEST_CASE ("arp : swing retarde les pas impairs", "[arp]")
{
    auto a = make();
    Params p = a.params();
    p.swing = 0.5;
    a.setParams (p);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 1), 512);
    std::vector<long long> ons;
    for (const auto& e : ev) if (e.on) ons.push_back (e.at);
    REQUIRE (ons.size() >= 4);
    const double step = 0.25 * kSamplesPerBeat;
    CHECK (ons[0] == 0);
    CHECK (ons[1] == (long long) std::llround (step * 1.5));
    CHECK (ons[2] == (long long) std::llround (step * 2));
    CHECK (ons[3] == (long long) std::llround (step * 3.5));
}

TEST_CASE ("arp : relâcher les touches arrête le motif, sans note bloquée", "[arp]")
{
    auto a = make();
    std::vector<Event> in = kChord;
    const long long t = (long long) (kSamplesPerBeat * 1.1);
    in.push_back ({ t, false, 60, 0 });
    in.push_back ({ t, false, 64, 0 });
    in.push_back ({ t, false, 67, 0 });
    const auto ev = run (a, in, (long long) (kSamplesPerBeat * 4), 333);
    std::map<int, int> balance;
    long long lastOn = 0;
    for (const auto& e : ev)
    {
        balance[e.note] += e.on ? 1 : -1;
        if (e.on) lastOn = e.at;
    }
    for (const auto& kv : balance) CHECK (kv.second == 0);
    CHECK (lastOn < t);
    CHECK (a.pendingCount() == 0);
}

TEST_CASE ("arp : maintien", "[arp]")
{
    auto a = make();
    Params p = a.params();
    p.hold = true;
    a.setParams (p);
    std::vector<Event> in = kChord;
    const long long t = 1000;
    in.push_back ({ t, false, 60, 0 });
    in.push_back ({ t, false, 64, 0 });
    in.push_back ({ t, false, 67, 0 });
    CHECK (onNotes (run (a, in, (long long) (kSamplesPerBeat * 1), 256)).size() == 4);
    // un nouvel accord remplace l'ancien
    const long long t2 = (long long) (kSamplesPerBeat * 1.01);
    std::vector<Event> in2 = { { t2, true, 50, 100 } };
    const auto ev = run (a, in2, (long long) (kSamplesPerBeat * 2), 256, true, 1.0);
    for (const auto& e : ev) if (e.on && e.at > t2 + 12000) CHECK (e.note == 50);
}

TEST_CASE ("arp : saut de position et arrêt de la lecture libèrent les notes", "[arp]")
{
    auto a = make();
    std::vector<NoteOut> out (256);
    NoteIn on { 0, true, 60, 100 };
    Transport tr;
    tr.playing = true;
    tr.hasPosition = true;
    tr.ppq = 0.0;
    int total = 0;
    total += a.process (&on, 1, tr, 100, out.data(), 256);   // un début à 0
    REQUIRE (total == 1);
    REQUIRE (out[0].on);
    tr.ppq = 32.0;   // saut : la fin en cours doit être émise tout de suite
    const int m = a.process (nullptr, 0, tr, 100, out.data(), 256);
    REQUIRE (m >= 1);
    CHECK (! out[0].on);
    CHECK (out[0].offset == 0);

    // arrêt de la lecture : retour à l'horloge interne, aucune note en suspens
    auto b = make();
    tr.ppq = 0.0;
    b.process (&on, 1, tr, 100, out.data(), 256);
    tr.playing = false;
    tr.hasPosition = false;
    const int k = b.process (nullptr, 0, tr, 100, out.data(), 256);
    REQUIRE (k >= 1);
    CHECK (! out[0].on);
}

TEST_CASE ("arp : joue aussi lecture arrêtée (horloge interne)", "[arp]")
{
    auto a = make();
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 2), 512, false);
    CHECK (onNotes (ev).size() >= 7);
}

TEST_CASE ("arp : vélocité fixe, accent, transposition", "[arp]")
{
    auto a = make();
    Params p = a.params();
    p.fixedVelocity = true;
    p.velocity = 80;
    p.accentEvery = 4;
    p.accentBoost = 30;
    p.transpose = 12;
    a.setParams (p);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 1.5), 256);
    int i = 0;
    for (const auto& e : ev)
        if (e.on)
        {
            CHECK (e.vel == ((i % 4) == 0 ? 110 : 80));
            CHECK (e.note >= 72);
            ++i;
        }
}

TEST_CASE ("arp : toujours autant de fins que de débuts, quels que soient les sauts", "[arp]")
{
    auto a = make (Pattern::Random, 1.0 / 16.0);
    Params p = a.params();
    p.gate = 1.0;
    p.swing = 0.7;
    a.setParams (p);
    std::map<int, int> balance;
    std::vector<NoteOut> out (4096);
    unsigned seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    double ppq = 0.0;
    for (int blockNo = 0; blockNo < 600; ++blockNo)
    {
        const int n = 1 + (int) (rnd() % 700);
        std::vector<NoteIn> in;
        if (rnd() % 5 == 0) in.push_back ({ (int) (rnd() % n), true, (std::uint8_t) (40 + rnd() % 40), 90 });
        if (rnd() % 7 == 0) in.push_back ({ (int) (rnd() % n), false, (std::uint8_t) (40 + rnd() % 40), 0 });
        std::stable_sort (in.begin(), in.end(), [] (const NoteIn& x, const NoteIn& y) { return x.offset < y.offset; });
        Transport tr;
        tr.playing = (blockNo % 50) < 40;
        tr.hasPosition = tr.playing;
        tr.bpm = 90.0 + (double) (rnd() % 90);
        if (rnd() % 40 == 0) ppq = (double) (rnd() % 64);   // saut
        tr.ppq = ppq;
        const int m = a.process (in.data(), (int) in.size(), tr, n, out.data(), 4096);
        for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
        ppq += n * tr.bpm / 60.0 / kSr;
    }
    a.reset();
    const int m = a.process (nullptr, 0, Transport {}, 64, out.data(), 4096);
    for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
    for (const auto& kv : balance) CHECK (kv.second == 0);
}

// ================================================================================================ mode motif

namespace
{
using neptune::core::pattern::Pattern;
Pattern patternFrom (const char* json)
{
    const auto r = neptune::core::pattern::fromJson (json);
    REQUIRE (r.ok);
    REQUIRE (r.patterns.size() == 1);
    return r.patterns[0];
}
const char* kArpeggioJson = R"({ "id": "a", "length": [1,2], "hits": [
    { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["3"] },
    { "at": [2,8], "dur": [1,8], "parts": ["5"] }, { "at": [3,8], "dur": [1,8], "parts": ["5+1"] } ] })";
}

TEST_CASE ("arp motif : degrés de l'accord tenu, répétition dans la mesure, instants exacts", "[arp][pattern]")
{
    const auto pat = patternFrom (kArpeggioJson);
    auto a = make();
    a.setPattern (&pat);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 4), 333);
    // un motif de 1/2 ronde (2 noires) dans une mesure de 4 noires : joué deux fois
    CHECK (onNotes (ev) == std::vector<int> { 60, 64, 67, 79, 60, 64, 67, 79 });
    int k = 0;
    for (const auto& e : ev)
        if (e.on) CHECK (e.at == (long long) std::llround (k++ * 0.5 * kSamplesPerBeat));
}

TEST_CASE ("arp motif : même résultat à toutes les tailles de blocs", "[arp][pattern]")
{
    const auto pat = patternFrom (kArpeggioJson);
    std::vector<Event> ref;
    for (int block : { 1, 7, 64, 256, 1000, 4096 })
    {
        auto a = make();
        a.setPattern (&pat);
        const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 8), block);
        if (ref.empty()) ref = ev;
        REQUIRE (ev.size() == ref.size());
        for (size_t i = 0; i < ev.size(); ++i)
        {
            CHECK (ev[i].at == ref[i].at);
            CHECK (ev[i].on == ref[i].on);
            CHECK (ev[i].note == ref[i].note);
        }
    }
}

TEST_CASE ("arp motif : accord renversé reconnu, basse sous la fondamentale", "[arp][pattern]")
{
    const auto pat = patternFrom (R"({ "id": "b", "length": [1,1], "hits": [
        { "at": [0,1], "dur": [1,4], "parts": ["B"] }, { "at": [1,4], "dur": [1,4], "parts": ["1"] },
        { "at": [2,4], "dur": [1,4], "parts": ["3"] }, { "at": [3,4], "dur": [1,4], "parts": ["C"] } ] })");
    auto a = make();
    a.setPattern (&pat);
    // mi sol do : do majeur en premier renversement (la basse est mi)
    const std::vector<Event> in = { { 0, true, 64, 90 }, { 0, true, 67, 90 }, { 0, true, 72, 90 } };
    const auto ons = onNotes (run (a, in, (long long) (kSamplesPerBeat * 3.9), 256));
    REQUIRE (ons.size() >= 6);
    CHECK (ons[0] == 64);          // la basse : mi, sous la fondamentale (do = 72)
    CHECK (ons[1] == 72);          // 1 = do
    CHECK (ons[2] == 76);          // 3 = mi
    CHECK (ons[3] == 72);          // l'accord entier : do, mi, sol
    CHECK (ons[4] == 76);
    CHECK (ons[5] == 79);
}

TEST_CASE ("arp motif : vélocité, accent, durée propres au coup", "[arp][pattern]")
{
    const auto pat = patternFrom (R"({ "id": "v", "length": [1,1], "hits": [
        { "at": [0,1], "dur": [1,4], "parts": ["1"], "vel": 110, "gate": 0.5 },
        { "at": [1,4], "dur": [1,4], "parts": ["1"], "accent": true },
        { "at": [2,4], "dur": [1,4], "parts": ["1"], "staccato": true } ] })");
    auto a = make();
    Params p = a.params();
    p.accentBoost = 20;
    p.gate = 1.0;
    a.setParams (p);
    a.setPattern (&pat);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 3), 256);
    std::vector<Event> ons;
    for (const auto& e : ev) if (e.on && e.note == 60) ons.push_back (e);
    REQUIRE (ons.size() >= 3);
    CHECK (ons[0].vel == 110);
    CHECK (ons[1].vel == 90 + 20);   // vélocité jouée (90) + force de l'accent
    CHECK (ons[2].vel == 90);        // le staccato est ignoré
    // fin du premier coup : gate 0,5 de sa longueur (1 noire)
    for (const auto& e : ev) if (! e.on && e.note == 60) { CHECK (e.at == (long long) (0.5 * kSamplesPerBeat)); break; }
}

TEST_CASE ("arp motif : deux mesures, la mesure du morceau choisit celle du motif", "[arp][pattern]")
{
    const auto pat = patternFrom (R"({ "id": "t", "length": [2,1], "bars": 2, "hits": [
        { "at": [0,1], "dur": [1,4], "parts": ["1"] }, { "at": [1,1], "dur": [1,4], "parts": ["3"] } ] })");
    auto a = make();
    a.setPattern (&pat);
    const auto ons = onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 12), 500));
    CHECK (ons == std::vector<int> { 60, 64, 60 });   // mesures 0, 1, 2 du morceau
}

TEST_CASE ("arp motif : pas de note bloquée, quels que soient les sauts", "[arp][pattern]")
{
    const auto pat = patternFrom (R"({ "id": "r", "length": [1,1], "hits": [
        { "at": [0,1], "dur": [1,4], "parts": ["C"] }, { "at": [1,12], "dur": [1,12], "parts": ["5"] },
        { "at": [1,2], "dur": [1,1], "parts": ["B","9"] } ] })");
    auto a = make();
    Params p = a.params();
    p.gate = 1.0;
    a.setParams (p);
    a.setPattern (&pat);
    std::map<int, int> balance;
    std::vector<NoteOut> out (4096);
    unsigned seed = 777;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    double ppq = 0.0;
    for (int blockNo = 0; blockNo < 600; ++blockNo)
    {
        const int n = 1 + (int) (rnd() % 900);
        std::vector<NoteIn> in;
        if (rnd() % 5 == 0) in.push_back ({ (int) (rnd() % n), true, (std::uint8_t) (40 + rnd() % 40), 90 });
        if (rnd() % 7 == 0) in.push_back ({ (int) (rnd() % n), false, (std::uint8_t) (40 + rnd() % 40), 0 });
        std::stable_sort (in.begin(), in.end(), [] (const NoteIn& x, const NoteIn& y) { return x.offset < y.offset; });
        Transport tr;
        tr.playing = (blockNo % 50) < 40;
        tr.hasPosition = tr.playing;
        tr.bpm = 70.0 + (double) (rnd() % 120);
        if (rnd() % 40 == 0) ppq = (double) (rnd() % 64);
        tr.ppq = ppq;
        const int m = a.process (in.data(), (int) in.size(), tr, n, out.data(), 4096);
        for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
        ppq += n * tr.bpm / 60.0 / kSr;
    }
    a.reset();
    const int m = a.process (nullptr, 0, Transport {}, 64, out.data(), 4096);
    for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
    for (const auto& kv : balance) CHECK (kv.second == 0);
}

TEST_CASE ("arp motif : mode notes, rangs des notes tenues, toujours un motif complet", "[arp][pattern]")
{
    const auto pat = patternFrom (R"({ "id": "n", "length": [1,2], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["2"] },
        { "at": [2,8], "dur": [1,8], "parts": ["3"] }, { "at": [3,8], "dur": [1,8], "parts": ["4"] } ] })");
    REQUIRE (pat.notesMode);
    auto a = make();
    a.setPattern (&pat);
    // trois notes : la quatrième remonte d'une octave ; deux répétitions dans la mesure
    CHECK (onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 4), 256)) == std::vector<int> { 60, 64, 67, 72, 60, 64, 67, 72 });
    // une seule note : le motif se promène par octaves
    auto b = make();
    b.setPattern (&pat);
    const std::vector<Event> one = { { 0, true, 50, 90 } };
    CHECK (onNotes (run (b, one, (long long) (kSamplesPerBeat * 2), 256)) == std::vector<int> { 50, 62, 74, 86 });
    // des notes qui ne forment aucun accord : les rangs marchent quand même
    auto c = make();
    c.setPattern (&pat);
    const std::vector<Event> odd = { { 0, true, 60, 90 }, { 0, true, 61, 90 }, { 0, true, 66, 90 } };
    CHECK (onNotes (run (c, odd, (long long) (kSamplesPerBeat * 2), 256)) == std::vector<int> { 60, 61, 66, 72 });
}

// ================================================================================================ environnement de Carte

namespace
{
using neptune::core::arp::Environment;
using neptune::core::arp::Harmony;

struct FakeEnv : Environment
{
    struct M { double start, length; };
    std::vector<M> measures;
    bool chord = false;
    Harmony hm;
    const Pattern* early = nullptr;
    const Pattern* late = nullptr;
    double switchAt = 1e9;

    bool hasPatterns() const noexcept override { return early != nullptr; }
    const Pattern* patternAt (double t) const noexcept override { return t < switchAt ? early : late; }
    bool harmonyAt (double, Harmony& out) const noexcept override { if (chord) out = hm; return chord; }
    bool nextMeasureStart (double t, double& start) const noexcept override
    {
        for (const auto& m : measures) if (m.start > t) { start = m.start; return true; }
        return false;
    }
    bool measureAt (double t, double& start, double& length, int& index) const noexcept override
    {
        for (size_t i = 0; i < measures.size(); ++i)
            if (t >= measures[i].start && t < measures[i].start + measures[i].length) { start = measures[i].start; length = measures[i].length; index = (int) i; return true; }
        return false;
    }
};
}

TEST_CASE ("arp carte : l'accord de Carte remplace la reconnaissance des notes tenues", "[arp][carte]")
{
    const auto pat = patternFrom (R"({ "id": "q", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["3"] },
        { "at": [2,4], "dur": [1,4], "parts": ["5"] }, { "at": [3,4], "dur": [1,4], "parts": ["7"] } ] })");
    FakeEnv env;
    env.chord = true;
    env.hm.rootPc = 2;                 // ré
    env.hm.rel = 0x489;                // m7 : 0 3 7 10
    auto a = make();
    a.setPattern (&pat);
    a.setEnvironment (&env);
    // une seule note tenue (do 3 = 48) : sert de repère d'octave ; l'accord vient de Carte
    const std::vector<Event> one = { { 0, true, 48, 90 } };
    CHECK (onNotes (run (a, one, (long long) (kSamplesPerBeat * 3.9), 256)) == std::vector<int> { 50, 53, 57, 60 });
    // sans Carte, la même note seule est « sa propre fondamentale »
    auto b = make();
    b.setPattern (&pat);
    CHECK (onNotes (run (b, one, (long long) (kSamplesPerBeat * 3.9), 256)) == std::vector<int> { 48 });   // 3, 5 et 7 absents d'un accord d'une note
}

TEST_CASE ("arp carte : la gamme donne la note d'un degré absent de l'accord", "[arp][carte]")
{
    const auto pat = patternFrom (R"({ "id": "g", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["7"] }, { "at": [2,4], "dur": [1,4], "parts": ["9"] } ] })");
    FakeEnv env;
    env.chord = true;
    env.hm.rootPc = 0;
    env.hm.rel = 0x091;                // do majeur (0 4 7)
    env.hm.hasScale = true;
    env.hm.scaleRel = 0x6B5;           // mixolydien : 0 2 4 5 7 9 10
    const std::vector<Event> in = { { 0, true, 60, 90 } };
    for (auto mode : { neptune::core::pattern::Absent::Skip, neptune::core::pattern::Absent::Play, neptune::core::pattern::Absent::Scale })
    {
        auto a = make();
        a.setPattern (&pat, mode);
        a.setEnvironment (&env);
        const auto ons = onNotes (run (a, in, (long long) (kSamplesPerBeat * 2.9), 256));
        if (mode == neptune::core::pattern::Absent::Skip) CHECK (ons == std::vector<int> { 60 });
        if (mode == neptune::core::pattern::Absent::Play) CHECK (ons == std::vector<int> { 60, 71, 74 });   // 7 majeure, 9 par défaut
        if (mode == neptune::core::pattern::Absent::Scale) CHECK (ons == std::vector<int> { 60, 70, 74 });  // la 7e du mixolydien : si bémol ; la 9e : ré
    }
}

TEST_CASE ("arp carte : mesures réelles (anacrouse) et motif de deux mesures", "[arp][carte]")
{
    const auto two = patternFrom (R"({ "id": "t", "length": [2,1], "bars": 2, "hits": [
        { "at": [0,1], "dur": [1,4], "parts": ["1"] }, { "at": [1,1], "dur": [1,4], "parts": ["3"] } ] })");
    FakeEnv env;
    env.measures = { { 0.0, 1.0 }, { 1.0, 4.0 }, { 5.0, 4.0 }, { 9.0, 3.0 } };   // anacrouse d'un temps, deux 4/4, un 3/4
    env.early = &two;
    auto a = make();
    a.setEnvironment (&env);
    const auto ev = run (a, kChord, (long long) (kSamplesPerBeat * 12), 300);
    std::vector<Event> ons;
    for (const auto& e : ev) if (e.on) ons.push_back (e);
    REQUIRE (ons.size() == 4);
    // mesure 0 (anacrouse) joue la mesure 0 du motif, 1 la mesure 1, 2 la mesure 0, 3 la mesure 1 (à son début)
    CHECK (ons[0].note == 60);
    CHECK (ons[0].at == 0);
    CHECK (ons[1].note == 64);
    CHECK (ons[1].at == (long long) std::llround (1.0 * kSamplesPerBeat));
    CHECK (ons[2].note == 60);
    CHECK (ons[2].at == (long long) std::llround (5.0 * kSamplesPerBeat));
    CHECK (ons[3].note == 64);
    CHECK (ons[3].at == (long long) std::llround (9.0 * kSamplesPerBeat));
}

TEST_CASE ("arp carte : le motif change quand Carte le demande, au début de la mesure", "[arp][carte]")
{
    const auto a1 = patternFrom (R"({ "id": "a", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,4], "parts": ["1"] } ] })");
    const auto b1 = patternFrom (R"({ "id": "b", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,4], "parts": ["5"] } ] })");
    FakeEnv env;
    env.early = &a1;
    env.late = &b1;
    env.switchAt = 8.0;
    auto a = make();
    a.setEnvironment (&env);
    for (int block : { 64, 1000 })
    {
        a.reset();
        const auto ons = onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 12), block));
        CHECK (ons == std::vector<int> { 60, 60, 67 });   // mesures 0 et 1 : motif A ; mesure 2 (à partir du temps 8) : motif B
    }
}

TEST_CASE ("arp carte : sans note tenue, rien ne sonne ; pas de note bloquée", "[arp][carte]")
{
    const auto pat = patternFrom (kArpeggioJson);
    FakeEnv env;
    env.chord = false;   // Carte n'a pas d'accord ici : rien à jouer sans note tenue
    env.early = &pat;
    auto a = make();
    a.setEnvironment (&env);
    CHECK (run (a, {}, (long long) (kSamplesPerBeat * 4), 256).empty());
    env.chord = true;
    std::map<int, int> balance;
    for (const auto& e : run (a, kChord, (long long) (kSamplesPerBeat * 3), 100)) balance[e.note] += e.on ? 1 : -1;
    a.reset();
    std::vector<NoteOut> out (64);
    const int m = a.process (nullptr, 0, Transport {}, 64, out.data(), 64);
    for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
    for (const auto& kv : balance) CHECK (kv.second == 0);
}

// ================================================================================================ de bout en bout : Carte écrit, Arpège lit

#include <atomic>
#include <chrono>

#include "neptune/core/CarteScore.h"
#include "neptune/core/ChordMapEnvironment.h"

TEST_CASE ("de bout en bout : la carte publiée par Carte pilote l'arpégiateur", "[arp][carte][integration]")
{
    using namespace neptune::core;
    static std::atomic<int> counter { 0 };
    const std::string tag = "arp" + std::to_string (static_cast<long> (std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)) + "-" + std::to_string (counter++) + "-";
    chordmap::Writer writer (tag);
    chordmap::Reader reader (tag);
    REQUIRE (writer.open (1) == chordmap::Status::Ok);

    // morceau : anacrouse d'un temps, puis Dm7 (une mesure) et G7 (une mesure) en ré dorien / sol mixolydien, motif 2 sur la 2e mesure
    carte::Score s;
    chords::Item dm7;
    dm7.start = 1.0; dm7.length = 4.0;
    REQUIRE (chords::parseChord ("Dm7", dm7.chord));
    chords::Item g7;
    g7.start = 5.0; g7.length = 4.0;
    REQUIRE (chords::parseChord ("G7", g7.chord));
    s.chords = { dm7, g7 };
    carte::placeScale (s.scales, { 1.0, 4.0, 2, 4, false });    // ré dorien
    carte::placeScale (s.scales, { 5.0, 4.0, 7, 7, false });    // sol mixolydien
    carte::placePattern (s.patterns, { 0.0, 5.0, 1 });          // motif 1 jusqu'au temps 5
    carte::placePattern (s.patterns, { 5.0, 4.0, 2 });          // motif 2 ensuite
    s.measures = { { 0.0, 1.0, 4, 4, 0 }, { 1.0, 4.0, 4, 4, 1 }, { 5.0, 4.0, 4, 4, 2 } };
    writer.publish (carte::toContainers (s));
    reader.poll();
    REQUIRE (reader.connected());

    const auto p1 = patternFrom (R"({ "id": "p1", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["3"] }, { "at": [2,4], "dur": [1,4], "parts": ["5"] }, { "at": [3,4], "dur": [1,4], "parts": ["7"] } ] })");
    const auto p2 = patternFrom (R"({ "id": "p2", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,1], "parts": ["B","C"] } ] })");
    arp::ChordMapEnvironment::Slots slots;
    slots[0] = std::make_shared<const neptune::core::pattern::Pattern> (p1);
    slots[1] = std::make_shared<const neptune::core::pattern::Pattern> (p2);
    arp::ChordMapEnvironment env (reader, slots);

    auto a = make();
    a.setEnvironment (&env);
    const std::vector<Event> held = { { 0, true, 48, 90 } };   // une seule note tenue : le repère d'octave
    const auto ev = run (a, held, (long long) (kSamplesPerBeat * 9), 480);
    std::vector<Event> ons;
    for (const auto& e : ev) if (e.on) ons.push_back (e);

    // anacrouse (temps 0 à 1) : motif 1 sur la mesure d'un temps ; pas d'accord de Carte avant le temps 1 : accord reconnu dans la note tenue (do seul) -> 1 seulement
    // mesure 1 (temps 1 à 5) : Dm7 en motif 1 (la mesure 1 du morceau est le rang 1, le motif a 1 mesure : alors toujours la même)
    // 4 coups : ré fa la do (50, 53, 57, 60), aux temps 1, 2, 3, 4
    // mesure 2 (temps 5 à 9) : motif 2 : basse + accord de G7 en une fois, au temps 5
    std::vector<int> expectMeasure1 = { 50, 53, 57, 60 };
    std::vector<int> got1;
    for (const auto& e : ons)
        if (e.at >= (long long) (1.0 * kSamplesPerBeat) && e.at < (long long) (5.0 * kSamplesPerBeat)) got1.push_back (e.note);
    CHECK (got1 == expectMeasure1);
    std::vector<int> got2;
    for (const auto& e : ons)
        if (e.at >= (long long) (5.0 * kSamplesPerBeat)) { got2.push_back (e.note); CHECK (e.at == (long long) (5.0 * kSamplesPerBeat)); }
    // G7 (sol si ré fa) : la fondamentale la plus proche de la note tenue (do 48) est sol 43 ; la basse est sol, une octave plus bas : 31 ; puis l'accord 43 47 50 53
    CHECK (got2 == std::vector<int> { 31, 43, 47, 50, 53 });
    // et les instants sont ceux des mesures réelles, indépendamment de la taille des blocs
    for (int block : { 1, 64, 1024 })
    {
        auto b = make();
        b.setEnvironment (&env);
        const auto ev2 = run (b, held, (long long) (kSamplesPerBeat * 9), block);
        CHECK (onNotes (ev2) == onNotes (ev));
    }
    writer.close();
}

TEST_CASE ("arp carte : Carte joue seule, sans note tenue, pendant la lecture", "[arp][carte]")
{
    FakeEnv env;
    env.chord = true;
    env.hm.rootPc = 2;                 // ré
    env.hm.rel = 0x489;                // m7 : 0 3 7 10
    const long long len = (long long) (kSamplesPerBeat * 1.0);   // 4 pas de double-croche
    SECTION ("motif automatique : l'accord de Carte est arpégé")
    {
        auto a = make();
        a.setEnvironment (&env);
        CHECK (onNotes (run (a, {}, len, 256)) == std::vector<int> { 50, 53, 57, 60 });
    }
    SECTION ("octave de base réglable")
    {
        auto a = make();
        a.setEnvironment (&env);
        a.setBaseNote (60);
        CHECK (onNotes (run (a, {}, len, 256)) == std::vector<int> { 62, 65, 69, 72 });
    }
    SECTION ("motif en degrés")
    {
        const auto pat = patternFrom (R"({ "id": "d", "length": [1,1], "hits": [
            { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["3"] },
            { "at": [2,4], "dur": [1,4], "parts": ["5"] }, { "at": [3,4], "dur": [1,4], "parts": ["7"] } ] })");
        auto a = make();
        a.setPattern (&pat);
        a.setEnvironment (&env);
        CHECK (onNotes (run (a, {}, (long long) (kSamplesPerBeat * 3.9), 256)) == std::vector<int> { 50, 53, 57, 60 });
    }
    SECTION ("motif en rangs : les notes de l'accord tiennent lieu de notes tenues")
    {
        const auto pat = patternFrom (R"({ "id": "r", "length": [1,1], "x": { "arpege": { "mode": "notes" } }, "hits": [
            { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["2"] },
            { "at": [2,4], "dur": [1,4], "parts": ["3"] }, { "at": [3,4], "dur": [1,4], "parts": ["4"] } ] })");
        auto a = make();
        a.setPattern (&pat);
        a.setEnvironment (&env);
        CHECK (onNotes (run (a, {}, (long long) (kSamplesPerBeat * 3.9), 256)) == std::vector<int> { 50, 53, 57, 60 });
    }
    SECTION ("lecture arrêtée : rien ne sonne (la position de Carte n'a pas de sens)")
    {
        auto a = make();
        a.setEnvironment (&env);
        CHECK (run (a, {}, len * 4, 256, false).empty());
    }
    SECTION ("des notes tenues l'emportent sur l'accord synthétisé")
    {
        auto a = make();
        a.setEnvironment (&env);
        CHECK (onNotes (run (a, kChord, len, 256)) == std::vector<int> { 60, 64, 67, 60 });
    }
    SECTION ("pas de note bloquée")
    {
        auto a = make();
        a.setEnvironment (&env);
        std::map<int, int> balance;
        for (const auto& e : run (a, {}, (long long) (kSamplesPerBeat * 2), 77)) balance[e.note] += e.on ? 1 : -1;
        a.reset();
        std::vector<NoteOut> out (64);
        const int m = a.process (nullptr, 0, Transport {}, 64, out.data(), 64);
        for (int i = 0; i < m; ++i) balance[out[i].note] += out[i].on ? 1 : -1;
        for (const auto& kv : balance) CHECK (kv.second == 0);
    }
}

TEST_CASE ("de bout en bout : une basse étrangère n'est pas une note de l'accord (Am7/D)", "[arp][carte][integration]")
{
    using namespace neptune::core;
    static std::atomic<int> counter { 100 };
    const std::string tag = "bas" + std::to_string (static_cast<long> (std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)) + "-" + std::to_string (counter++) + "-";
    chordmap::Writer writer (tag);
    chordmap::Reader reader (tag);
    REQUIRE (writer.open (1) == chordmap::Status::Ok);
    carte::Score s;
    chords::Item it;
    it.start = 0.0; it.length = 8.0;
    REQUIRE (chords::parseChord ("Am7/D", it.chord));
    s.chords = { it };
    writer.publish (carte::toContainers (s));
    reader.poll();
    arp::ChordMapEnvironment::Slots slots;
    arp::ChordMapEnvironment env (reader, slots);
    arp::Harmony hm;
    REQUIRE (env.harmonyAt (1.0, hm));
    CHECK (hm.rootPc == 9);
    CHECK (hm.bassPc == 2);
    CHECK (hm.rel == 0x489);   // la m7 : 0 3 7 10, sans le ré
    writer.close();
}

TEST_CASE ("arp : variantes de motif (alts), tirage reproductible et indépendant des blocs", "[arp][pattern]")
{
    const char* json = R"({ "id": "va", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["1"] },
        { "at": [2,4], "dur": [1,4], "parts": ["1"] }, { "at": [3,4], "dur": [1,4], "parts": ["1"] } ],
        "alts": [ { "id": "fill", "chance": %s, "from": [1,2], "hits": [
            { "at": [2,4], "dur": [1,8], "parts": ["5"] }, { "at": [5,8], "dur": [1,8], "parts": ["5"] }, { "at": [3,4], "dur": [1,4], "parts": ["5+1"] } ] } ] })";
    auto make1 = [&] (const char* chance) { char buf[1024]; std::snprintf (buf, sizeof buf, json, chance); return patternFrom (buf); };
    const long long len = (long long) (kSamplesPerBeat * 4 * 8 - 10);   // huit mesures

    SECTION ("chance 0 : la variante ne joue jamais")
    {
        const auto pat = make1 ("0");
        REQUIRE (pat.alts.size() == 1);
        auto a = make();
        a.setPattern (&pat);
        const auto ons = onNotes (run (a, kChord, len, 512));
        CHECK (ons.size() == 32);
        for (int n : ons) CHECK (n == 60);
    }
    SECTION ("chance 1 : la variante remplace la fin de chaque mesure")
    {
        const auto pat = make1 ("1");
        auto a = make();
        a.setPattern (&pat);
        const auto ons = onNotes (run (a, kChord, (long long) (kSamplesPerBeat * 4 - 10), 512));
        CHECK (ons == std::vector<int> { 60, 60, 67, 67, 79 });   // 2 coups d'origine, puis 5, 5 et 5+1 de la variante
    }
    SECTION ("chance 0,5 : un mélange, le même à chaque lecture et à toute taille de bloc")
    {
        const auto pat = make1 ("0.5");
        std::vector<int> ref;
        for (int block : { 1, 64, 512, 4096 })
        {
            auto a = make();
            a.setPattern (&pat);
            const auto ons = onNotes (run (a, kChord, len, block));
            if (ref.empty()) ref = ons;
            CHECK (ons == ref);
        }
        int plain = 0, filled = 0;
        for (size_t i = 0; i + 3 < ref.size(); ++i) { (void) i; }
        for (int n : ref) { if (n == 60) ++plain; else ++filled; }
        CHECK (plain > 8);
        CHECK (filled > 4);   // au moins une mesure a pris la variante, au moins une ne l'a pas prise
    }
}

TEST_CASE ("de bout en bout : le morceau d'essai de Carte et les motifs d'essai d'Arpège", "[arp][carte][integration]")
{
    using namespace neptune::core;
    static std::atomic<int> counter { 200 };
    const std::string tag = "ess" + std::to_string (static_cast<long> (std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)) + "-" + std::to_string (counter++) + "-";
    chordmap::Writer writer (tag);
    chordmap::Reader reader (tag);
    REQUIRE (writer.open (1) == chordmap::Status::Ok);
    writer.publish (carte::toContainers (carte::demoScore()));
    reader.poll();
    REQUIRE (reader.connected());
    CHECK (reader.count (chordmap::Kind::Measure) == 8);

    std::ifstream in (std::string (NEPTUNE_SOURCE_DIR) + "/docs/exemples-arpege/motifs-essai.json");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto pats = neptune::core::pattern::fromJson (ss.str());
    REQUIRE (pats.ok);
    arp::ChordMapEnvironment::Slots slots;
    for (size_t i = 0; i < pats.patterns.size(); ++i) slots[11 + i] = std::make_shared<const neptune::core::pattern::Pattern> (pats.patterns[i]);
    arp::ChordMapEnvironment env (reader, slots);

    auto a = make();
    a.setEnvironment (&env);
    const auto ev = run (a, {}, (long long) (kSamplesPerBeat * 31), 480);   // les 8 mesures, sans note tenue
    std::vector<Event> ons;
    for (const auto& e : ev) if (e.on) ons.push_back (e);
    REQUIRE (ons.size() > 40);

    // mesure 1 (Essai 1, mode notes, do majeur : do mi sol, base do 48) : 1 2 3 4 3 2 3 puis 1+1 ; vélocités 50..120, accents (+20) sur les 3e et 7e
    const std::vector<int> notes1 = { 48, 52, 55, 60, 55, 52, 55, 60 };
    const std::vector<int> vel1 = { 50, 65, 100, 95, 110, 95, 100, 120 };
    for (size_t i = 0; i < 8; ++i)
    {
        CHECK (ons[i].note == notes1[i]);
        CHECK (ons[i].vel == vel1[i]);
        CHECK (ons[i].at == (long long) std::llround (i * 0.5 * kSamplesPerBeat));
    }
    // mesure 5 (Essai 3, ballade, Dm7) : basse et accord au temps 16 ; la basse est ré, une octave sous la fondamentale
    long long beat16 = (long long) std::llround (16.0 * kSamplesPerBeat);
    std::vector<int> atBeat16;
    for (const auto& e : ons) if (e.at == beat16) atBeat16.push_back (e.note);
    REQUIRE (atBeat16.size() == 5);                    // la basse + les quatre notes de Dm7
    CHECK (atBeat16[0] == 38);                          // ré 38 = ré 50 - 12 ; (le tri place la basse la première : même instant, ordre de création)
    // mesure 6 (Am7/D) : la basse est ré (pas la)
    const long long beat20 = (long long) std::llround (20.0 * kSamplesPerBeat);
    bool bassD = false;
    for (const auto& e : ons) if (e.at == beat20 && e.note % 12 == 2 && e.note < 57) bassD = true;   // ré sous la fondamentale (la = 57)
    CHECK (bassD);
    // mesure 8 (3/4) : Essai 4, 2e mesure du motif, coupée à la barre : aucun début au-delà de la fin du morceau
    const long long end = (long long) std::llround (31.0 * kSamplesPerBeat);
    for (const auto& e : ons) CHECK (e.at < end);
    writer.close();
}


// ================================================================================================ suiveur d'accords (Orgamon)

#include "neptune/core/ChordFollower.h"

namespace
{
struct ChordListEnv : neptune::core::arp::Environment
{
    struct C { double start, length; int root; std::uint16_t rel; int bass; };
    std::vector<C> chords;
    bool harmonyAt (double t, neptune::core::arp::Harmony& out) const noexcept override
    {
        for (const auto& c : chords)
            if (t >= c.start && t < c.start + c.length)
            {
                out.rootPc = c.root; out.rel = c.rel; out.bassPc = c.bass; out.startBeats = c.start; out.lengthBeats = c.length;
                return true;
            }
        return false;
    }
    bool nextChordStart (double t, double& start) const noexcept override
    {
        for (const auto& c : chords) if (c.start > t) { start = c.start; return true; }
        return false;
    }
};

struct FEv { long long at; bool on; int ch, note; };
std::vector<FEv> runFollower (neptune::core::arp::ChordFollower& f, const neptune::core::arp::Environment& env, long long total, int block, bool playing = true)
{
    using namespace neptune::core::arp;
    std::vector<FEv> v;
    std::vector<FollowEvent> out (64);
    for (long long pos = 0; pos < total; pos += block)
    {
        const int n = (int) std::min<long long> (block, total - pos);
        const int m = f.process (env, playing, playing, (double) pos / kSamplesPerBeat, kBpm, kSr, n, out.data(), (int) out.size());
        for (int i = 0; i < m; ++i) v.push_back ({ pos + out[(size_t) i].offset, out[(size_t) i].on, out[(size_t) i].channel, out[(size_t) i].note });
    }
    return v;
}
}

TEST_CASE ("suiveur : l'accord de Carte est tenu, change à l'échantillon près", "[arp][carte][follower]")
{
    ChordListEnv env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 7, 0x491, -1 } };   // do majeur, puis sol7 (0 4 7 10)
    neptune::core::arp::ChordFollower f;
    f.setChordChannel (0);
    const auto ev = runFollower (f, env, (long long) (kSamplesPerBeat * 7.9), 480);
    std::vector<int> on0, on1, off1;
    for (const auto& e : ev)
    {
        if (e.on && e.at == 0) on0.push_back (e.note);
        if (e.at == (long long) (4 * kSamplesPerBeat)) (e.on ? on1 : off1).push_back (e.note);
    }
    CHECK (on0 == std::vector<int> { 48, 52, 55 });                 // do mi sol, octave de base 48
    CHECK (off1 == std::vector<int> { 48, 52, 55 });                // relâchés exactement au temps 4
    CHECK (on1 == std::vector<int> { 55, 59, 62, 65 });             // sol si ré fa (sol placé entre 48 et 59)
    // même résultat à toute taille de bloc
    for (int block : { 1, 64, 1000, 4096 })
    {
        neptune::core::arp::ChordFollower g;
        const auto ev2 = runFollower (g, env, (long long) (kSamplesPerBeat * 7.9), block);
        REQUIRE (ev2.size() == ev.size());
        for (size_t i = 0; i < ev.size(); ++i) { CHECK (ev2[i].at == ev[i].at); CHECK (ev2[i].note == ev[i].note); CHECK (ev2[i].on == ev[i].on); }
    }
}

TEST_CASE ("suiveur : basse, trou entre deux accords, arrêt de la lecture", "[arp][carte][follower]")
{
    ChordListEnv env;
    env.chords = { { 0.0, 2.0, 9, 0x489, 2 }, { 4.0, 2.0, 0, 0x091, -1 } };    // Am7/D, un trou de deux temps, puis do
    neptune::core::arp::ChordFollower f;
    f.setChordChannel (0);
    f.setBassChannel (2);
    const auto ev = runFollower (f, env, (long long) (kSamplesPerBeat * 6.5), 512);
    int bass = -1;
    for (const auto& e : ev) if (e.on && e.ch == 2 && e.at == 0) bass = e.note;
    CHECK (bass == 38);                                                    // la basse est ré (pas la), sous l'octave de base
    int offsAt2 = 0, onsBetween = 0;
    for (const auto& e : ev)
    {
        if (! e.on && e.at == (long long) (2 * kSamplesPerBeat)) ++offsAt2;   // tout est relâché à la fin du premier accord
        if (e.on && e.at > (long long) (2 * kSamplesPerBeat) && e.at < (long long) (4 * kSamplesPerBeat)) ++onsBetween;   // rien ne sonne dans le trou
    }
    CHECK (offsAt2 == 5);                                                  // quatre notes de l'accord + la basse
    CHECK (onsBetween == 0);
    // arrêt de la lecture : tout est relâché
    neptune::core::arp::ChordFollower g;
    runFollower (g, env, (long long) (kSamplesPerBeat * 1.0), 256);
    CHECK (g.sounding());
    std::vector<neptune::core::arp::FollowEvent> out (64);
    const int m = g.process (env, false, false, 0.0, kBpm, kSr, 256, out.data(), 64);
    CHECK (m == 4);
    CHECK (! g.sounding());
    for (int i = 0; i < m; ++i) CHECK (! out[(size_t) i].on);
}

TEST_CASE ("suiveur : voix conduites, peu de mouvement d'un accord à l'autre, basse proche", "[arp][carte][follower][voice]")
{
    ChordListEnv env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 5, 0x0A1, -1 }, { 8.0, 4.0, 7, 0x491, -1 }, { 12.0, 4.0, 0, 0x091, -1 } };   // do, fa, sol7, do
    neptune::core::arp::ChordFollower f;
    f.setVoiceLeading (true);
    f.setChordChannel (0);
    f.setBassChannel (1);
    const auto ev = runFollower (f, env, (long long) (kSamplesPerBeat * 15.9), 480);
    std::vector<std::vector<int>> hands (4);
    std::vector<int> basses;
    for (const auto& e : ev)
    {
        if (! e.on) continue;
        const int k = (int) (e.at / (4 * kSamplesPerBeat));
        if (e.ch == 0) hands[(size_t) k].push_back (e.note);
        else basses.push_back (e.note);
    }
    REQUIRE (basses.size() == 4);
    for (size_t k = 0; k < 4; ++k)
    {
        auto h = hands[k];
        std::sort (h.begin(), h.end());
        for (int n : h) { CHECK (n >= 55); CHECK (n <= 76); }
        if (k > 0)
        {
            auto p = hands[k - 1];
            std::sort (p.begin(), p.end());
            int move = 0;
            if (p.size() == h.size()) for (size_t i = 0; i < h.size(); ++i) move += std::abs (h[i] - p[i]);
            CHECK (move <= 8);
        }
        if (k > 0) CHECK (std::abs (basses[k] - basses[k - 1]) <= 7);
    }
    // à l'arrêt puis au redémarrage, la mémoire est effacée : même position qu'au premier départ
    std::vector<neptune::core::arp::FollowEvent> out (64);
    f.process (env, false, false, 0.0, kBpm, kSr, 256, out.data(), 64);
    const auto again = runFollower (f, env, (long long) (kSamplesPerBeat * 1.0), 480);
    std::vector<int> a0;
    for (const auto& e : again) if (e.on && e.ch == 0) a0.push_back (e.note);
    CHECK (a0 == hands[0]);
}

TEST_CASE ("motif : voices.rh = led est lu", "[arp][pattern][voice]")
{
    const auto r = neptune::core::pattern::fromJson (R"({"id":"a","length":[1,1],"voices":{"rh":"led","lh":"literal"},"hits":[{"at":[0,1],"dur":[1,4],"parts":["C"]}]})");
    REQUIRE (r.ok);
    CHECK (r.patterns[0].voicesLed);
    const auto r2 = neptune::core::pattern::fromJson (R"({"id":"a","length":[1,1],"hits":[{"at":[0,1],"dur":[1,4],"parts":["C"]}]})");
    CHECK (! r2.patterns[0].voicesLed);
}

TEST_CASE ("arp : voix conduites, l'accord suivant se pose au plus près du précédent", "[arp][carte][voice]")
{
    const auto pat = patternFrom (R"({ "id": "c", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,1], "parts": ["C"] } ] })");
    ChordListEnv env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 5, 0x091, -1 }, { 8.0, 4.0, 7, 0x491, -1 } };   // do, fa, sol7
    auto spread = [&] (bool led)
    {
        auto a = make();
        a.setPattern (&pat);
        a.setEnvironment (&env);
        a.setVoiceLeading (led);
        const auto ons = onNotes (run (a, { { 0, true, 48, 90 } }, (long long) (kSamplesPerBeat * 11.9), 256));
        REQUIRE (ons.size() >= 9);
        return ons;
    };
    const auto fixedNotes = spread (false), ledNotes = spread (true);
    auto topMove = [] (const std::vector<int>& v)
    {
        int worst = 0, prevTop = -1;
        size_t i = 0;
        while (i < v.size())
        {
            const size_t n = i == 0 ? 3 : (v.size() - i >= 4 && i >= 6 ? 4 : 3);
            int top = 0;
            for (size_t k = i; k < i + n && k < v.size(); ++k) top = std::max (top, v[k]);
            if (prevTop >= 0) worst = std::max (worst, std::abs (top - prevTop));
            prevTop = top;
            i += n;
        }
        return worst;
    };
    CHECK (topMove (ledNotes) <= topMove (fixedNotes));
    for (int n : ledNotes) { CHECK (n >= 55); CHECK (n <= 76); }
}
