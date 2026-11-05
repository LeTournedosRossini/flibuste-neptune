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
