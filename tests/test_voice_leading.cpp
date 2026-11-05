// Conduite de voix : port du solveur de Scorsaire (cas de référence de ses tests `audio-comping` et `comping-voices`).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <vector>

#include "neptune/core/VoiceLeading.h"

using namespace neptune::core::voice;

namespace
{
std::vector<int> notes (const Voicing& v) { return { v.notes.begin(), v.notes.begin() + v.count }; }
std::vector<int> pcsOf (const Voicing& v)
{
    std::vector<int> o;
    for (int i = 0; i < v.count; ++i) o.push_back (v.notes[(size_t) i] % 12);
    std::sort (o.begin(), o.end());
    return o;
}
constexpr std::uint16_t bit (int s) { return (std::uint16_t) (1u << s); }
const std::uint16_t kMaj = bit (0) | bit (4) | bit (7);
const std::uint16_t kDom7 = bit (0) | bit (4) | bit (7) | bit (10);
Voicing lead (const Voicing* prev, int root, std::uint16_t rel, const Rules& r = {}, int bass = -1, int prevBass = -1)
{
    int pcs[kMaxVoices];
    const int n = chordTones (root, rel, pcs);
    return voiceLead (prev ? prev->notes.data() : nullptr, prev ? prev->count : 0, pcs, n, kHandLow, kHandHigh, r, bass, prevBass);
}
}

TEST_CASE ("voix : écartement des positions", "[voice]")
{
    auto shape = [] (std::vector<int> v) { return voicingShape (v.data(), (int) v.size()); };
    CHECK (shape ({ 60, 64, 67, 71 }) == Shape::Close);
    CHECK (shape ({ 60, 67, 76 }) == Shape::Drop2);
    CHECK (shape ({ 59, 65, 67, 74 }) == Shape::Drop2);
    CHECK (shape ({ 52, 60, 67, 71 }) == Shape::Drop3);
    CHECK (shape ({ 48, 50, 70 }) == Shape::Other);
    CHECK (shape ({ 60, 64 }) == Shape::Close);
}

TEST_CASE ("voix : notes de l'accord, la quinte tombe au-delà de quatre", "[voice]")
{
    int pcs[kMaxVoices];
    const std::uint16_t ninth = bit (0) | bit (4) | bit (7) | bit (10) | bit (2);
    int n = chordTones (0, ninth, pcs);
    std::vector<int> v (pcs, pcs + n);
    std::sort (v.begin(), v.end());
    CHECK (v == std::vector<int> { 0, 2, 4, 10 });
    CHECK (chordTones (0, kMaj, pcs) == 3);
    CHECK (chordTones (0, kMaj, pcs, 0) == 2);   // sans la basse
}

TEST_CASE ("voix : do puis fa bouge peu, dans la tessiture", "[voice]")
{
    const auto c = lead (nullptr, 0, kMaj);
    const auto f = lead (&c, 5, kMaj);
    CHECK (pcsOf (f) == std::vector<int> { 0, 5, 9 });
    int move = 0;
    for (int i = 0; i < 3; ++i) move += std::abs (f.notes[(size_t) i] - c.notes[(size_t) i]);
    CHECK (move <= 5);
    for (int i = 0; i < 3; ++i) { CHECK (c.notes[(size_t) i] >= kHandLow); CHECK (f.notes[(size_t) i] <= kHandHigh); }
}

TEST_CASE ("voix : une suite d'accords ne dérive pas", "[voice]")
{
    Voicing prev;
    bool first = true;
    const int roots[] = { 0, 5, 7, 0, 5, 7, 0 };
    for (int r : roots)
    {
        prev = lead (first ? nullptr : &prev, r, r == 7 ? kDom7 : kMaj);
        first = false;
        for (int i = 0; i < prev.count; ++i) { CHECK (prev.notes[(size_t) i] >= kHandLow); CHECK (prev.notes[(size_t) i] <= kHandHigh); }
    }
}

TEST_CASE ("voix : basse la plus proche de la précédente", "[voice]")
{
    CHECK (bassNote (0) == 48);
    CHECK (std::abs (bassNote (7, 48) - 48) <= 6);
    CHECK (bassNote (7, 48) == 43);
}

TEST_CASE ("voix : coût, mouvement contraire et parallèles", "[voice]")
{
    const int prev[] = { 64, 67 }, now[] = { 65, 69 };
    Context ctx;
    ctx.prev = prev;
    ctx.prevCount = 2;
    ctx.bass = 62;
    ctx.prevBass = 60;
    const double base = voicingCost (now, 2, {}, ctx);
    Rules contrary; contrary.contrary = true;
    Rules par; par.noParallels = true;
    CHECK_THAT (voicingCost (now, 2, contrary, ctx) - base, Catch::Matchers::WithinAbs (costs().sameWay, 1e-9));
    CHECK_THAT (voicingCost (now, 2, par, ctx) - base, Catch::Matchers::WithinAbs (costs().parallel, 1e-9));
    ctx.bass = 58;   // les voix s'écartent : pas de pénalité
    Rules both; both.contrary = both.noParallels = true;
    CHECK (voicingCost (now, 2, both, ctx) == voicingCost (now, 2, {}, ctx));
}

TEST_CASE ("voix : la voix du haut saute moins quand la souplesse monte", "[voice]")
{
    const int prev[] = { 60, 64, 67, 71 }, leap[] = { 60, 65, 69, 76 }, calm[] = { 60, 65, 69, 72 };
    Context ctx;
    ctx.prev = prev;
    ctx.prevCount = 4;
    auto diff = [&] (double smooth) { Rules r; r.topSmooth = smooth; return voicingCost (leap, 4, r, ctx) - voicingCost (calm, 4, r, ctx); };
    CHECK (diff (2) > diff (0));
}

TEST_CASE ("voix : écartement demandé respecté", "[voice]")
{
    Rules r = Rules::usual();
    r.spacing = Spacing::Drop2;
    Voicing prev;
    bool first = true;
    int drop2 = 0;
    for (int root : { 0, 5, 7, 0, 9 })
    {
        prev = lead (first ? nullptr : &prev, root, kDom7, r);
        first = false;
        if (voicingShape (prev.notes.data(), prev.count) == Shape::Drop2) ++drop2;
    }
    CHECK (drop2 >= 3);
}

TEST_CASE ("voix : sans règle, simple moindre mouvement ; les secondes sont évitées avec la règle", "[voice]")
{
    const auto c = lead (nullptr, 0, kMaj);
    const auto f = lead (&c, 5, kMaj);
    CHECK (c.count == 3);
    CHECK (f.count == 3);
    // do majeur 7 : si et do voisins en haut seraient une seconde mineure
    const std::uint16_t maj7 = bit (0) | bit (4) | bit (7) | bit (11);
    const auto v = lead (nullptr, 0, maj7, Rules::usual());
    for (int i = 0; i + 1 < v.count; ++i) CHECK (v.notes[(size_t) i + 1] - v.notes[(size_t) i] != 1);
}

TEST_CASE ("voix : classe sans note dans la tessiture = vide", "[voice]")
{
    const int pcs[] = { 0, 4 };
    CHECK (voiceLead (nullptr, 0, pcs, 2, 60, 62).empty());
    CHECK (voiceLead (nullptr, 0, pcs, 0).empty());
}
