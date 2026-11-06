// Réserve d'automation : emplacements assignables, refus des doublons, verrou, synchro dans les deux sens, état.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "neptune/core/ParamReserve.h"

using namespace neptune::core;
using Catch::Matchers::WithinAbs;

namespace
{
struct Rig
{
    ParameterSet slots;    // les emplacements 0..7 (ids 100..107)
    ParameterSet target;   // trois réglages : 1 continu, 2 à crans (quatre choix), 3 continu
    ParamReserve reserve { 8 };
    Rig()
    {
        for (int k = 0; k < 8; ++k) slots.add (ParamSpec::continuous (100u + static_cast<ParamId> (k), "Slot", Unit::None, 0.0, 1.0, 0.0));
        target.add (ParamSpec::continuous (1, "A", Unit::Decibel, -60.0, 12.0, 0.0));
        target.add (ParamSpec::choice (2, "B", { "a", "b", "c", "d" }, 0));
        target.add (ParamSpec::continuous (3, "C", Unit::Hertz, 20.0, 20000.0, 1000.0, Curve::Exponential));
    }
    ParamReserve::Ref resolve (std::uint32_t, std::uint32_t, ParamId id)
    {
        const auto i = target.indexOf (id);
        return i == ParameterSet::npos ? ParamReserve::Ref {} : ParamReserve::Ref { &target, i };
    }
    void sync() { reserve.sync (slots, 0, [this] (std::uint32_t s, std::uint32_t u, ParamId p) { return resolve (s, u, p); }); }
    void resync() { reserve.resync (slots, 0, [this] (std::uint32_t s, std::uint32_t u, ParamId p) { return resolve (s, u, p); }); }
    double slotN (int k) { return slots.getNormalizedAt (static_cast<std::size_t> (k)); }
    void setSlotN (int k, double v) { slots.setNormalizedAt (static_cast<std::size_t> (k), v); }
    double targetN (ParamId id) { return target.getNormalizedAt (target.indexOf (id)); }
    // assigne en recopiant la valeur du réglage dans l'emplacement, comme le fait le plugin
    ParamReserve::Result assign (int k, ParamId id, bool replace = false, int* takenBy = nullptr)
    {
        const auto r = reserve.assign (k, 0, 0, id, targetN (id), replace, takenBy);
        if (r == ParamReserve::Result::Ok) setSlotN (k, targetN (id));
        return r;
    }
};
} // namespace

TEST_CASE ("réserve : l'assignation prend la valeur du réglage, sans saut", "[reserve]")
{
    Rig r;
    r.target.setNormalizedAt (r.target.indexOf (1), 0.25);
    REQUIRE (r.assign (3, 1) == ParamReserve::Result::Ok);
    CHECK_THAT (r.slotN (3), WithinAbs (0.25, 1e-12));
    r.sync();
    CHECK_THAT (r.targetN (1), WithinAbs (0.25, 1e-12));   // rien n'a bougé
    CHECK (r.reserve.target (3).active);
    CHECK (r.reserve.slotOf (0, 0, 1) == 3);
    CHECK (r.reserve.usedCount() == 1);
}

TEST_CASE ("réserve : l'emplacement pilote le réglage, le réglage suit l'emplacement", "[reserve]")
{
    Rig r;
    REQUIRE (r.assign (0, 1) == ParamReserve::Result::Ok);
    r.setSlotN (0, 0.8);                       // l'hôte pousse l'emplacement
    r.sync();
    CHECK_THAT (r.targetN (1), WithinAbs (0.8, 1e-12));
    // le potard bouge à l'écran : l'emplacement le suit, et le dit une fois
    r.target.setNormalizedAt (r.target.indexOf (1), 0.3);
    r.sync();
    CHECK_THAT (r.slotN (0), WithinAbs (0.3, 1e-12));
    CHECK (r.reserve.takeFollowed (0));
    CHECK_FALSE (r.reserve.takeFollowed (0));
    // pas de boucle : un bloc de plus ne change rien
    r.sync();
    CHECK_THAT (r.slotN (0), WithinAbs (0.3, 1e-12));
    CHECK_THAT (r.targetN (1), WithinAbs (0.3, 1e-12));
    CHECK_FALSE (r.reserve.takeFollowed (0));
}

TEST_CASE ("réserve : si l'hôte et l'écran bougent ensemble, l'hôte l'emporte", "[reserve]")
{
    Rig r;
    REQUIRE (r.assign (1, 1) == ParamReserve::Result::Ok);
    r.setSlotN (1, 0.9);
    r.target.setNormalizedAt (r.target.indexOf (1), 0.1);
    r.sync();
    CHECK_THAT (r.targetN (1), WithinAbs (0.9, 1e-12));
    CHECK_THAT (r.slotN (1), WithinAbs (0.9, 1e-12));
}

TEST_CASE ("réserve : un réglage à crans garde ses crans", "[reserve]")
{
    Rig r;
    r.target.setNormalizedAt (r.target.indexOf (2), 0.0);
    REQUIRE (r.assign (2, 2) == ParamReserve::Result::Ok);
    r.setSlotN (2, 0.7);                       // entre deux crans (quatre choix : 0, 1/3, 2/3, 1)
    r.sync();
    const double snapped = r.targetN (2);
    CHECK_THAT (snapped, WithinAbs (2.0 / 3.0, 1e-9));
    CHECK_THAT (r.target.getAt (r.target.indexOf (2)), WithinAbs (2.0, 1e-9));
    // l'emplacement garde la valeur de l'hôte : pas de bagarre, pas de bascule au bloc suivant
    CHECK_THAT (r.slotN (2), WithinAbs (0.7, 1e-12));
    r.sync();
    CHECK_THAT (r.targetN (2), WithinAbs (snapped, 1e-12));
    CHECK_THAT (r.slotN (2), WithinAbs (0.7, 1e-12));
}

TEST_CASE ("réserve : un réglage n'a qu'un emplacement, un emplacement occupé ne se réassigne pas sans accord", "[reserve]")
{
    Rig r;
    REQUIRE (r.assign (0, 1) == ParamReserve::Result::Ok);
    int taken = -1;
    CHECK (r.assign (1, 1, false, &taken) == ParamReserve::Result::TargetTaken);   // ce réglage est déjà sur l'emplacement 0
    CHECK (taken == 0);
    CHECK_FALSE (r.reserve.target (1).active);
    CHECK (r.assign (0, 3) == ParamReserve::Result::SlotTaken);                    // l'emplacement 0 est occupé
    CHECK (r.reserve.target (0).param == 1u);                                      // rien n'a changé
    CHECK (r.assign (0, 3, true) == ParamReserve::Result::Ok);                     // avec accord : réassigné
    CHECK (r.reserve.target (0).param == 3u);
    CHECK (r.reserve.slotOf (0, 0, 1) == -1);
    CHECK (r.assign (0, 3) == ParamReserve::Result::Ok);                           // même cible, même emplacement : rien à refuser
}

TEST_CASE ("réserve : emplacement libre, retrait, hors limites", "[reserve]")
{
    Rig r;
    CHECK (r.reserve.firstFree() == 0);
    REQUIRE (r.assign (0, 1) == ParamReserve::Result::Ok);
    REQUIRE (r.assign (1, 2) == ParamReserve::Result::Ok);
    CHECK (r.reserve.firstFree() == 2);
    CHECK (r.reserve.unassign (0) == ParamReserve::Result::Ok);
    CHECK (r.reserve.firstFree() == 0);
    // retiré : le réglage ne suit plus l'emplacement
    r.setSlotN (0, 0.9);
    const double before = r.targetN (1);
    r.sync();
    CHECK_THAT (r.targetN (1), WithinAbs (before, 1e-12));
    CHECK (r.assign (9, 1) == ParamReserve::Result::BadSlot);
    CHECK (r.assign (-1, 1) == ParamReserve::Result::BadSlot);
    CHECK (r.reserve.unassign (8) == ParamReserve::Result::BadSlot);
    // plein : plus de place libre
    for (int k = 0; k < 8; ++k) r.reserve.assign (k, 0, static_cast<std::uint32_t> (k), 1, 0.0);
    CHECK (r.reserve.firstFree() == -1);
}

TEST_CASE ("réserve : un emplacement verrouillé ne se réassigne ni ne se libère", "[reserve]")
{
    Rig r;
    REQUIRE (r.assign (4, 1) == ParamReserve::Result::Ok);
    r.reserve.setLocked (4, true);
    CHECK (r.reserve.locked (4));
    CHECK (r.assign (4, 3, true) == ParamReserve::Result::Locked);
    CHECK (r.reserve.unassign (4) == ParamReserve::Result::Locked);
    CHECK (r.reserve.target (4).param == 1u);
    r.reserve.setLocked (4, false);
    CHECK (r.reserve.unassign (4) == ParamReserve::Result::Ok);
}

TEST_CASE ("réserve : un réglage disparu est retiré", "[reserve]")
{
    Rig r;
    REQUIRE (r.assign (0, 1) == ParamReserve::Result::Ok);
    REQUIRE (r.assign (1, 3) == ParamReserve::Result::Ok);
    r.reserve.prune ([] (const ParamReserve::Target& t) { return t.param != 3u; });
    CHECK (r.reserve.target (0).active);
    CHECK_FALSE (r.reserve.target (1).active);
}

TEST_CASE ("réserve : un réglage qui n'existe plus est ignoré par le fil audio", "[reserve]")
{
    Rig r;
    REQUIRE (r.reserve.assign (0, 0, 0, 999, 0.5) == ParamReserve::Result::Ok);   // id inconnu du jeu de réglages
    r.setSlotN (0, 0.7);
    r.sync();   // ne plante pas
    CHECK_THAT (r.slotN (0), WithinAbs (0.7, 1e-12));
}

TEST_CASE ("réserve : état, aller-retour avec verrous, sans écraser les réglages rappelés", "[reserve][state]")
{
    Rig a;
    a.target.setNormalizedAt (a.target.indexOf (1), 0.4);
    REQUIRE (a.assign (2, 1) == ParamReserve::Result::Ok);
    REQUIRE (a.assign (5, 3) == ParamReserve::Result::Ok);
    a.reserve.setLocked (5, true);
    a.reserve.setLocked (7, true);   // verrou sur un emplacement libre
    const auto json = a.reserve.toJson();

    Rig b;
    auto exists = [&b] (const ParamReserve::Target& t) { return b.target.indexOf (t.param) != ParameterSet::npos; };
    auto current = [&b] (const ParamReserve::Target& t) { return b.targetN (t.param); };
    b.reserve.fromJson (json, exists, current);
    CHECK (b.reserve.target (2).active);
    CHECK (b.reserve.target (2).param == 1u);
    CHECK (b.reserve.target (5).param == 3u);
    CHECK (b.reserve.locked (5));
    CHECK (b.reserve.locked (7));
    CHECK_FALSE (b.reserve.target (7).active);
    CHECK_FALSE (b.reserve.target (0).active);
    // le rappel d'un état d'emplacements et de réglages : la synchro ne les écrase pas
    b.setSlotN (2, 0.6);
    b.target.setNormalizedAt (b.target.indexOf (1), 0.35);
    b.resync();
    b.sync();
    CHECK_THAT (b.targetN (1), WithinAbs (0.35, 1e-12));
    CHECK_THAT (b.slotN (2), WithinAbs (0.6, 1e-12));
    // un état avec un réglage inconnu : l'entrée est ignorée
    auto bad = json;
    bad[2]["param"] = 4242;
    Rig c;
    c.reserve.fromJson (bad, [&c] (const ParamReserve::Target& t) { return c.target.indexOf (t.param) != ParameterSet::npos; }, [&c] (const ParamReserve::Target&) { return 0.0; });
    CHECK_FALSE (c.reserve.target (2).active);
    CHECK (c.reserve.target (5).active);
    // un état illisible ne casse rien
    c.reserve.fromJson (nlohmann::json ("pas un tableau"), [] (const ParamReserve::Target&) { return true; }, [] (const ParamReserve::Target&) { return 0.0; });
    CHECK (c.reserve.usedCount() == 0);
}

TEST_CASE ("réserve : 256 emplacements tiennent et ne se mélangent pas", "[reserve]")
{
    ParameterSet slots, target;
    for (int k = 0; k < 256; ++k) slots.add (ParamSpec::continuous (1000u + static_cast<ParamId> (k), "S", Unit::None, 0.0, 1.0, 0.0));
    for (int k = 0; k < 256; ++k) target.add (ParamSpec::continuous (1u + static_cast<ParamId> (k), "T", Unit::None, 0.0, 1.0, 0.0));
    ParamReserve res (256);
    for (int k = 0; k < 256; ++k) REQUIRE (res.assign (k, 7, 9, 1u + static_cast<ParamId> (k), 0.0) == ParamReserve::Result::Ok);
    CHECK (res.firstFree() == -1);
    slots.setNormalizedAt (200, 0.5);
    res.sync (slots, 0, [&] (std::uint32_t, std::uint32_t, ParamId id) { return ParamReserve::Ref { &target, target.indexOf (id) }; });
    for (int k = 0; k < 256; ++k) CHECK_THAT (target.getNormalizedAt (static_cast<std::size_t> (k)), WithinAbs (k == 200 ? 0.5 : 0.0, 1e-12));
    CHECK (res.target (255).scope == 7u);
    CHECK (res.target (255).sub == 9u);
}
