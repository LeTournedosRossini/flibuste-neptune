// Aperçu d'Arpège : rejouer l'arpégiateur hors ligne sur une carte d'accords et rendre les notes qui seraient lues.
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "neptune/core/ArpeggioPreview.h"

using namespace neptune::core;

namespace
{
struct TwoChords : arp::Environment
{
    struct C { double start, length; int root; std::uint32_t rel; int bass; };
    std::vector<C> chords;
    bool harmonyAt (double t, arp::Harmony& out) const noexcept override
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
std::set<int> pcsOf (const arp::Preview& p, double from, double to)
{
    std::set<int> r;
    for (const auto& n : p.notes) if (n.start >= from && n.start < to) r.insert (n.note % 12);
    return r;
}
}

TEST_CASE ("arp preview: nothing without a map, and the chords of the map are listed", "[arp][preview]")
{
    arp::PreviewSettings s;
    CHECK (arp::renderPreview (s).notes.empty());
    TwoChords env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 5, 0x091, -1 } };
    s.env = &env;
    const auto pv = arp::renderPreview (s);
    REQUIRE (pv.chords.size() == 2);
    CHECK (pv.chords[0].start == 0.0);
    CHECK (pv.chords[1].harmony.rootPc == 5);
    CHECK (pv.start == 0.0);
    CHECK (pv.end == 8.0);
    CHECK (! pv.truncated);
}

TEST_CASE ("arp preview: the notes follow the chords, with their ends", "[arp][preview]")
{
    TwoChords env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 5, 0x091, -1 } };   // do puis fa
    arp::PreviewSettings s;
    s.env = &env;
    s.params.stepBeats = 0.5;
    const auto pv = arp::renderPreview (s);
    REQUIRE (! pv.notes.empty());
    CHECK (pcsOf (pv, 0.0, 4.0) == std::set<int> { 0, 4, 7 });
    CHECK (pcsOf (pv, 4.0, 8.0) == std::set<int> { 5, 9, 0 });
    for (const auto& n : pv.notes)
    {
        CHECK (n.end > n.start);
        CHECK (n.start >= 0.0);
        CHECK (n.start < 8.0);
    }
}

TEST_CASE ("arp preview: a ninth chord plays its ninth in the octave above", "[arp][preview]")
{
    TwoChords env;
    env.chords = { { 0.0, 4.0, 0, 1u | 1u << 4 | 1u << 7 | 1u << 10 | 1u << 14, -1 } };
    arp::PreviewSettings s;
    s.env = &env;
    s.params.stepBeats = 0.25;
    s.params.octaves = 1;
    const auto pv = arp::renderPreview (s);
    int lo = 127, hi = 0;
    for (const auto& n : pv.notes) { lo = std::min (lo, n.note); hi = std::max (hi, n.note); }
    CHECK (pcsOf (pv, 0.0, 4.0).count (2) == 1);   // le ré
    CHECK (hi - lo >= 14);
}

TEST_CASE ("arp preview: approach chords are marked and listed", "[arp][preview][approach]")
{
    const auto r = pattern::fromJson (R"({ "id": "t", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["1"] },
        { "at": [2,4], "dur": [1,4], "parts": ["1"] }, { "at": [3,4], "dur": [1,4], "parts": ["1"] } ] })");
    REQUIRE (r.ok);
    TwoChords env;
    env.chords = { { 0.0, 4.0, 0, 0x091, -1 }, { 4.0, 4.0, 7, 0x091, -1 } };
    arp::PreviewSettings s;
    s.env = &env;
    s.pattern = &r.patterns[0];
    s.approach.kind = arp::ApproachParams::Kind::Dominant;   // V7 de sol = ré
    auto pv = arp::renderPreview (s);
    int marked = 0;
    for (const auto& n : pv.notes) if (n.approach) ++marked;
    CHECK (marked >= 1);
    REQUIRE (pv.chords.size() == 2);
    CHECK (pv.chords[0].hasApproach);
    CHECK (pv.chords[0].approach.rootPc == 2);
    CHECK (! pv.chords[1].hasApproach);
    s.approach.kind = arp::ApproachParams::Kind::Off;
    pv = arp::renderPreview (s);
    for (const auto& n : pv.notes) CHECK (! n.approach);
    CHECK (! pv.chords[0].hasApproach);
}

TEST_CASE ("arp preview: long maps are cut and flagged", "[arp][preview]")
{
    TwoChords env;
    for (int i = 0; i < 100; ++i) env.chords.push_back ({ i * 4.0, 4.0, i % 12, 0x091, -1 });
    arp::PreviewSettings s;
    s.env = &env;
    const auto pv = arp::renderPreview (s, 0.0, 64.0);
    CHECK (pv.truncated);
    CHECK (pv.end <= 64.0 + 1e-9);
}
