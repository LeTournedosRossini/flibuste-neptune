// Arpégiateur : motifs, instants à l'échantillon près, blocs de toute taille, arrêt / saut / boucle sans note bloquée.
#include <catch2/catch_test_macros.hpp>

#include <map>
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
    env.chord = true;
    env.early = &pat;
    auto a = make();
    a.setEnvironment (&env);
    CHECK (run (a, {}, (long long) (kSamplesPerBeat * 4), 256).empty());
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
