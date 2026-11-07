// Couche de contrôle : assignations (CC, OSC, macros) -> paramètres et actions, reprise, apprentissage, enregistrement, temps réel.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <set>
#include <thread>
#include <vector>

#include "neptune/core/Control.h"

using namespace neptune::core;
using namespace neptune::core::control;
using Catch::Approx;

namespace
{
constexpr ParamId kVolume = 10, kCutoff = 20, kToggle = 30, kStep = 40, kUnknown = 999;

struct Rig
{
    ParameterSet set;
    ControlEngine engine;
    Rig()
    {
        REQUIRE (set.add (ParamSpec::continuous (kVolume, "Volume", Unit::Decibel, -60.0, 6.0, 0.0)));
        REQUIRE (set.add (ParamSpec::continuous (kCutoff, "Cutoff", Unit::Hertz, 20.0, 20000.0, 1000.0, Curve::Exponential)));
        REQUIRE (set.add (ParamSpec::toggle (kToggle, "Toggle")));
        REQUIRE (set.add (ParamSpec::stepped (kStep, "Step", Unit::None, 0, 8, 0)));
    }
    void configure (std::vector<Binding> b, bool cc = true)
    {
        Config c;
        c.settings.ccEnabled = cc;
        c.bindings = std::move (b);
        engine.configure (c, set);
    }
};

Binding cc (int channel, int number, ParamId target)
{
    Binding b;
    b.source.kind = SourceKind::CC;
    b.source.channel = channel;
    b.source.number = number;
    b.target.param = target;
    return b;
}
} // namespace

TEST_CASE ("control: values are inverted, curved and confined to a sub-range", "[control]")
{
    Binding b;
    CHECK (mapValue (b, 0.25) == Approx (0.25));
    b.invert = true;
    CHECK (mapValue (b, 0.25) == Approx (0.75));
    b.invert = false;
    b.curve = 2.0;
    CHECK (mapValue (b, 0.5) == Approx (0.25));
    b.curve = 1.0;
    b.lo = 0.2;
    b.hi = 0.6;
    CHECK (mapValue (b, 0.0) == Approx (0.2));
    CHECK (mapValue (b, 1.0) == Approx (0.6));
    CHECK (mapValue (b, 0.5) == Approx (0.4));
    b.lo = 0.8;
    b.hi = 0.2;        // sous-intervalle à l'envers : la course est inversée
    CHECK (mapValue (b, 0.0) == Approx (0.8));
    CHECK (mapValue (b, 1.0) == Approx (0.2));
    CHECK (mapValue (b, 5.0) == Approx (0.2)); // valeur de source hors plage : bornée
}

TEST_CASE ("control: CC does nothing until enabled, then moves the target parameter", "[control]")
{
    Rig r;
    r.configure ({ cc (0, 7, kVolume) }, false);
    r.engine.controlChange (0, 7, 127, r.set);
    CHECK (r.set.get (kVolume) == Approx (0.0));      // désactivé par défaut : aucun effet

    r.configure ({ cc (0, 7, kVolume) }, true);
    r.engine.controlChange (0, 7, 127, r.set);
    CHECK (r.set.get (kVolume) == Approx (6.0));
    r.engine.controlChange (0, 7, 0, r.set);
    CHECK (r.set.get (kVolume) == Approx (-60.0));
    r.engine.controlChange (0, 8, 127, r.set);        // un autre numéro : rien
    CHECK (r.set.get (kVolume) == Approx (-60.0));
}

TEST_CASE ("control: channel filter, omni, several targets and several sources", "[control]")
{
    Rig r;
    r.configure ({ cc (3, 1, kVolume), cc (0, 2, kVolume), cc (0, 2, kCutoff), cc (1, 5, kStep) });
    r.engine.controlChange (0, 1, 127, r.set);        // voie 1 : l'assignation est sur la voie 3
    CHECK (r.set.get (kVolume) == Approx (0.0));
    r.engine.controlChange (2, 1, 127, r.set);        // voie 3 (indice 2)
    CHECK (r.set.get (kVolume) == Approx (6.0));
    r.engine.controlChange (15, 2, 0, r.set);         // omni : n'importe quelle voie, et vise deux paramètres
    CHECK (r.set.get (kVolume) == Approx (-60.0));
    CHECK (r.set.get (kCutoff) == Approx (20.0));
    r.engine.controlChange (0, 5, 127, r.set);
    CHECK (r.set.get (kStep) == 8.0);                 // paramètre discret : arrondi
}

TEST_CASE ("control: pickup waits for the source to reach the parameter", "[control]")
{
    Rig r;
    auto b = cc (0, 7, kVolume);
    b.mode = Mode::Pickup;
    r.configure ({ b });
    r.set.set (kVolume, 0.0);                          // à 90,9 % de la course (-60..+6)
    r.engine.controlChange (0, 7, 10, r.set);          // le fader est loin en bas : le paramètre ne bouge pas
    CHECK (r.set.get (kVolume) == Approx (0.0));
    r.engine.controlChange (0, 7, 100, r.set);         // encore loin (78 %)
    CHECK (r.set.get (kVolume) == Approx (0.0));
    r.engine.controlChange (0, 7, 116, r.set);         // 91,3 % : à la hauteur du paramètre -> il suit
    CHECK (r.set.getNormalized (kVolume) == Approx (116.0 / 127.0).margin (0.005));
    r.engine.controlChange (0, 7, 20, r.set);          // une fois repris, il suit partout
    CHECK (r.set.getNormalized (kVolume) == Approx (20.0 / 127.0).margin (0.005));
    r.engine.resetPickup();                            // changement de preset : on attend de nouveau
    r.set.set (kVolume, 0.0);
    r.engine.controlChange (0, 7, 5, r.set);
    CHECK (r.set.get (kVolume) == Approx (0.0));
}

TEST_CASE ("control: macros fan out to their targets", "[control]")
{
    Rig r;
    Binding a;
    a.source.kind = SourceKind::Macro;
    a.source.number = 3;
    a.target.param = kCutoff;
    Binding b = a;
    b.target.param = kVolume;
    b.invert = true;
    r.configure ({ a, b }, false);                     // les macros ne dépendent pas de l'interrupteur des CC
    r.engine.macroChanged (3, 1.0, r.set);
    CHECK (r.set.get (kCutoff) == Approx (20000.0));
    CHECK (r.set.get (kVolume) == Approx (-60.0));
    r.engine.macroChanged (4, 0.0, r.set);              // une autre macro : rien
    CHECK (r.set.get (kCutoff) == Approx (20000.0));
}

TEST_CASE ("control: OSC addresses reach their targets without touching the audio path", "[control]")
{
    Rig r;
    Binding b;
    b.source.kind = SourceKind::Osc;
    b.source.address = "/neptune/volume";
    b.target.param = kVolume;
    r.configure ({ b }, false);
    CHECK (r.engine.oscValue ("/neptune/volume", 1.0, r.set) == 1);
    CHECK (r.set.get (kVolume) == Approx (6.0));
    CHECK (r.engine.oscValue ("/neptune/other", 1.0, r.set) == 0);
    CHECK (r.engine.oscAddresses() == std::vector<std::string> { "/neptune/volume" });
}

TEST_CASE ("control: actions fire once per press, in order, on the message thread side", "[control]")
{
    Rig r;
    Binding next = cc (0, 20, 0), prev = cc (0, 21, 0), panic = cc (0, 22, 0);
    for (auto* b : { &next, &prev, &panic }) { b->target.isAction = true; b->target.param = 0; }
    next.target.action = Action::PresetNext;
    prev.target.action = Action::PresetPrevious;
    panic.target.action = Action::Panic;
    r.configure ({ next, prev, panic });
    r.engine.controlChange (0, 20, 127, r.set);
    r.engine.controlChange (0, 20, 127, r.set);        // toujours appuyé : pas de second déclenchement
    r.engine.controlChange (0, 20, 0, r.set);          // relâché
    r.engine.controlChange (0, 20, 100, r.set);        // nouvel appui
    r.engine.controlChange (0, 22, 127, r.set);
    Action a;
    REQUIRE (r.engine.popAction (a));
    CHECK (a == Action::PresetNext);
    REQUIRE (r.engine.popAction (a));
    CHECK (a == Action::PresetNext);
    REQUIRE (r.engine.popAction (a));
    CHECK (a == Action::Panic);
    CHECK_FALSE (r.engine.popAction (a));
    r.engine.controlChange (0, 21, 127, r.set);
    REQUIRE (r.engine.popAction (a));
    CHECK (a == Action::PresetPrevious);
}

TEST_CASE ("control: learning remembers the last CC, changes are reported once and grouped", "[control]")
{
    Rig r;
    r.configure ({ cc (0, 7, kVolume), cc (0, 8, kCutoff) });
    int ch = 0, num = 0;
    r.engine.controlChange (4, 75, 10, r.set);
    CHECK_FALSE (r.engine.takeLearnedCc (ch, num));   // l'apprentissage n'est pas actif
    r.engine.setLearning (true);
    r.engine.controlChange (4, 75, 10, r.set);
    REQUIRE (r.engine.takeLearnedCc (ch, num));
    CHECK (ch == 5);
    CHECK (num == 75);
    CHECK_FALSE (r.engine.takeLearnedCc (ch, num));

    std::vector<std::size_t> changed;
    r.engine.drainChanges (changed);                  // rien n'a changé jusqu'ici
    CHECK (changed.empty());
    for (int v = 0; v < 100; ++v) r.engine.controlChange (0, 7, v, r.set); // un balayage : un seul signalement pour le paramètre
    r.engine.controlChange (0, 8, 127, r.set);
    r.engine.drainChanges (changed);
    CHECK (std::set<std::size_t> (changed.begin(), changed.end()) == std::set<std::size_t> { r.set.indexOf (kVolume), r.set.indexOf (kCutoff) });
    CHECK (changed.size() == 2);
    changed.clear();
    CHECK (r.engine.drainChanges (changed) == 0);
}

TEST_CASE ("control: assignments on unknown parameters are kept but inert", "[control]")
{
    Rig r;
    r.configure ({ cc (0, 7, kUnknown), cc (0, 7, kVolume) });
    r.engine.controlChange (0, 7, 127, r.set);
    CHECK (r.set.get (kVolume) == Approx (6.0));
    CHECK (r.engine.config().bindings.size() == 2);    // l'assignation orpheline reste dans le projet
}

TEST_CASE ("control: configuration round trips, is sanitised and everything is off by default", "[control]")
{
    Config empty;
    CHECK_FALSE (empty.settings.ccEnabled);
    CHECK_FALSE (empty.settings.oscEnabled);
    CHECK (configFromJson ("pas du json") == Config {});
    CHECK_FALSE (configFromJson ("{}").settings.ccEnabled);

    Config c;
    c.settings.ccEnabled = true;
    c.settings.oscPort = 9100;
    c.settings.feedbackHost = "192.168.1.20";
    c.settings.feedbackPort = 9001;
    auto b = cc (2, 74, kCutoff);
    b.lo = 0.1; b.hi = 0.9; b.curve = 2.0; b.invert = true; b.mode = Mode::Pickup;
    c.bindings.push_back (b);
    Binding m;
    m.source.kind = SourceKind::Macro; m.source.number = 255; m.target.param = kVolume;
    c.bindings.push_back (m);
    Binding o;
    o.source.kind = SourceKind::Osc; o.source.address = "/a/b"; o.target.isAction = true; o.target.action = Action::TrackNext;
    c.bindings.push_back (o);
    CHECK (configFromJson (toJson (c)) == c);

    // assignations invalides ignorées, bornes appliquées
    const char* bad = R"({"bindings":[
        {"source":{"type":"cc","channel":0,"number":200},"target":{"param":10}},
        {"source":{"type":"macro","index":300},"target":{"param":10}},
        {"source":{"type":"osc","address":"sans-barre"},"target":{"param":10}},
        {"source":{"type":"cc","channel":0,"number":1},"target":{"action":"inconnue"}},
        {"source":{"type":"cc","channel":0,"number":1},"target":{"param":0}},
        {"source":{"type":"cc","channel":99,"number":5},"target":{"param":10},"curve":-4,"min":-1,"max":7}
    ],"osc_port":22})";
    const auto s = configFromJson (bad);
    REQUIRE (s.bindings.size() == 1);
    CHECK (s.bindings[0].source.channel == 16);
    CHECK (s.bindings[0].curve == Approx (0.1));
    CHECK (s.bindings[0].lo == 0.0);
    CHECK (s.bindings[0].hi == 1.0);
    CHECK (s.settings.oscPort == 1024);
}

TEST_CASE ("control: reconfiguring while the audio side is running is safe and takes effect", "[control]")
{
    Rig r;
    r.configure ({ cc (0, 7, kVolume) });
    std::atomic<bool> stop { false };
    std::thread audio ([&] { int v = 0; while (! stop.load()) { r.engine.controlChange (0, 7, (v++) & 127, r.set); r.engine.controlChange (0, 8, 5, r.set); } });
    for (int i = 0; i < 300; ++i)
    {
        r.configure ({ cc (0, 7, kVolume), cc (0, 8, kCutoff) });
        r.engine.collectGarbage();
        std::vector<std::size_t> changed;
        r.engine.drainChanges (changed);
    }
    stop.store (true);
    audio.join();
    r.engine.collectGarbage();
    r.engine.controlChange (0, 8, 127, r.set);
    CHECK (r.set.get (kCutoff) == Approx (20000.0));
}

TEST_CASE ("control: the macro pool declares 256 parameters and a macro moves its targets", "[control]")
{
    ParameterSet set;
    REQUIRE (addMacroParams (set));
    CHECK (set.size() == static_cast<std::size_t> (kMacros));
    CHECK (set.find (macroParamId (0)) != nullptr);
    CHECK (set.find (macroParamId (255)) != nullptr);
    CHECK (set.find (macroParamId (256)) == nullptr);
    CHECK (set.find (macroParamId (5))->name == "Macro 6");
    CHECK (set.get (macroParamId (3)) == Approx (0.0));

    REQUIRE (set.add (ParamSpec::continuous (kVolume, "Volume", Unit::Decibel, -60.0, 6.0, 0.0)));
    ControlEngine engine;
    Config c;
    Binding b;
    b.source.kind = SourceKind::Macro;
    b.source.number = 2;
    b.target.param = kVolume;
    c.bindings.push_back (b);
    engine.configure (c, set);
    engine.macroChanged (2, 1.0, set);
    CHECK (set.get (kVolume) == Approx (6.0));
    engine.macroChanged (3, 0.0, set);      // une autre macro ne touche rien
    CHECK (set.get (kVolume) == Approx (6.0));
}

TEST_CASE ("control: the reduced host list keeps strips, master and macros only", "[control]")
{
    CHECK (isBandOrMacroParam (1));                  // volume du master
    CHECK (isBandOrMacroParam (1000));               // volume de la piste 1
    CHECK_FALSE (isBandOrMacroParam (1010 + 100 * 3)); // bypass d'insert 1 de la piste 4 : par les macros
    CHECK_FALSE (isBandOrMacroParam (1027));           // niveau d'envoi 8 : par les macros
    CHECK (isBandOrMacroParam (1003 + 100 * 3));       // solo de la piste 4
    CHECK_FALSE (isBandOrMacroParam (1004));           // type de piste
    CHECK (isBandOrMacroParam (3100));               // volume du bus 2
    CHECK (isBandOrMacroParam (5003 + 100 * 31));    // solo de la dernière piste parallèle
    CHECK (isBandOrMacroParam (macroParamId (10)));
    CHECK_FALSE (isBandOrMacroParam (100));          // gain du sampler (piste 1)
    CHECK_FALSE (isBandOrMacroParam (1030));         // paramètre d'instrument
    CHECK_FALSE (isBandOrMacroParam (1040));         // synthé
    CHECK_FALSE (isBandOrMacroParam (9033));         // orgue
    CHECK_FALSE (isBandOrMacroParam (12000));        // emplacements
    CHECK_FALSE (isBandOrMacroParam (13256));
}

TEST_CASE ("control: source labels are short and language independent", "[control]")
{
    Source s;
    s.kind = SourceKind::CC; s.number = 74;
    CHECK (sourceLabel (s) == "CC 74");
    s.channel = 2;
    CHECK (sourceLabel (s) == "CC 74 ch 2");
    s.kind = SourceKind::Macro; s.number = 2;
    CHECK (sourceLabel (s) == "Macro 3");
    s.kind = SourceKind::Osc; s.address = "/a/b";
    CHECK (sourceLabel (s) == "OSC /a/b");
}

TEST_CASE ("control: NRPN selects a 14-bit parameter and moves its target", "[control]")
{
    Rig r;
    Binding b;
    b.source.kind = SourceKind::Nrpn;
    b.source.channel = 0;
    b.source.number = 1 * 128 + 5;      // MSB 1, LSB 5
    b.target.param = kVolume;
    r.configure ({ b }, true);
    r.engine.controlChange (0, 99, 1, r.set);
    r.engine.controlChange (0, 98, 5, r.set);
    r.engine.controlChange (0, 6, 127, r.set);
    r.engine.controlChange (0, 38, 127, r.set);       // 127 * 128 + 127 = 16383
    CHECK (r.set.getNormalized (kVolume) == Approx (1.0));
    r.engine.controlChange (0, 6, 64, r.set);         // 64 * 128 = 8192
    r.engine.controlChange (0, 38, 0, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (8192.0 / 16383.0).margin (1e-6));

    r.engine.controlChange (0, 99, 1, r.set);         // un autre paramètre : pas d'effet
    r.engine.controlChange (0, 98, 6, r.set);
    r.engine.controlChange (0, 6, 0, r.set);
    r.engine.controlChange (0, 38, 0, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (8192.0 / 16383.0).margin (1e-6));

    r.engine.controlChange (0, 101, 0, r.set);        // RPN : la sélection NRPN est annulée
    r.engine.controlChange (0, 6, 100, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (8192.0 / 16383.0).margin (1e-6));

    r.configure ({ b }, false);                        // désactivé : aucun effet
    r.engine.controlChange (0, 99, 1, r.set);
    r.engine.controlChange (0, 98, 5, r.set);
    r.engine.controlChange (0, 6, 0, r.set);
    r.engine.controlChange (0, 38, 0, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (8192.0 / 16383.0).margin (1e-6));
}

TEST_CASE ("control: NRPN can be learned even while controllers are disabled", "[control]")
{
    Rig r;
    r.configure ({}, false);
    r.engine.setLearning (true);
    r.engine.controlChange (2, 99, 3, r.set);
    r.engine.controlChange (2, 98, 9, r.set);
    r.engine.controlChange (2, 6, 10, r.set);
    int ch = 0, n = 0;
    CHECK (r.engine.takeLearnedNrpn (ch, n));
    CHECK (ch == 3);
    CHECK (n == 3 * 128 + 9);
    CHECK_FALSE (r.engine.takeLearnedCc (ch, n));      // les CC 99/98/6 ne sont pas appris comme des CC simples
    r.engine.controlChange (2, 74, 10, r.set);
    CHECK (r.engine.takeLearnedCc (ch, n));
    CHECK (n == 74);
}

TEST_CASE ("control: relative encoders add increments to the parameter", "[control]")
{
    CHECK (relativeDelta (Mode::RelativeTwos, 1) == 1);
    CHECK (relativeDelta (Mode::RelativeTwos, 127) == -1);
    CHECK (relativeDelta (Mode::RelativeTwos, 65) == -63);
    CHECK (relativeDelta (Mode::RelativeTwos, 0) == 0);
    CHECK (relativeDelta (Mode::RelativeOffset, 64) == 0);
    CHECK (relativeDelta (Mode::RelativeOffset, 66) == 2);
    CHECK (relativeDelta (Mode::RelativeOffset, 61) == -3);
    CHECK (relativeDelta (Mode::RelativeSign, 5) == 5);
    CHECK (relativeDelta (Mode::RelativeSign, 0x40 | 5) == -5);

    Rig r;
    auto b = cc (0, 16, kVolume);
    b.mode = Mode::RelativeTwos;
    r.configure ({ b }, true);
    CHECK (r.set.getNormalized (kVolume) == Approx (60.0 / 66.0));    // défaut 0 dB sur -60..+6
    const double start = r.set.getNormalized (kVolume);
    r.engine.controlChange (0, 16, 127, r.set);                        // -1 cran
    CHECK (r.set.getNormalized (kVolume) == Approx (start - 1.0 / 127.0));
    for (int i = 0; i < 400; ++i) r.engine.controlChange (0, 16, 1, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (1.0));             // borné
    b.lo = 0.2; b.hi = 0.6;
    r.configure ({ b }, true);
    for (int i = 0; i < 400; ++i) r.engine.controlChange (0, 16, 1, r.set);
    CHECK (r.set.getNormalized (kVolume) == Approx (0.6));             // borné au sous-intervalle
    std::vector<std::size_t> changed;
    CHECK (r.engine.drainChanges (changed) >= 1);
}

TEST_CASE ("control: relative mode and NRPN round trip in JSON, relative is limited to CC and parameters", "[control]")
{
    Config c;
    auto b = cc (3, 16, kVolume);
    b.mode = Mode::RelativeOffset;
    c.bindings.push_back (b);
    Binding n;
    n.source.kind = SourceKind::Nrpn; n.source.channel = 2; n.source.number = 16383; n.target.param = kCutoff;
    c.bindings.push_back (n);
    Binding bad = cc (0, 1, kVolume);
    bad.source.kind = SourceKind::Macro; bad.source.number = 1; bad.mode = Mode::RelativeSign;
    c.bindings.push_back (bad);
    Binding act = cc (0, 2, kVolume);
    act.target.isAction = true; act.target.action = Action::Panic; act.target.param = 0; act.mode = Mode::RelativeTwos;
    c.bindings.push_back (act);
    const auto back = configFromJson (toJson (c));
    REQUIRE (back.bindings.size() == 4);
    CHECK (back.bindings[0].mode == Mode::RelativeOffset);
    CHECK (back.bindings[1].source.kind == SourceKind::Nrpn);
    CHECK (back.bindings[1].source.number == 16383);
    CHECK (back.bindings[2].mode == Mode::Absolute);
    CHECK (back.bindings[3].mode == Mode::Absolute);
    CHECK (sourceLabel (back.bindings[1].source) == "NRPN 16383 ch 2");
    Binding out = n;
    out.source.number = 16384;
    Config c2; c2.bindings.push_back (out);
    CHECK (sanitised (c2).bindings.empty());
}

// ---- cibles d'insert : le réglage d'un effet posé sur une tranche, piloté par une macro, un CC ou OSC ----
namespace
{
// Un « module » : son jeu de paramètres, gardé en vie par la table de recherche qui le vise.
struct FakeModule
{
    ParameterSet set;
    FakeModule()
    {
        REQUIRE (set.add (ParamSpec::continuous (kVolume, "Threshold", Unit::Decibel, -60.0, 0.0, -20.0)));
    }
};

Target insertTarget (ParamId param, int strip = 0, int index = 0, int slot = 1, const char* type = "compressor")
{
    Target t;
    t.param = param;
    t.insertStrip = strip;
    t.insertIndex = index;
    t.insertSlot = slot;
    t.insertType = type;
    return t;
}

Binding macroToInsert (int macro, const Target& t)
{
    Binding b;
    b.source.kind = SourceKind::Macro;
    b.source.number = macro;
    b.target = t;
    return b;
}
} // namespace

TEST_CASE ("control: an insert target is written to the module, not to the Neptune parameter with the same number", "[control][insert]")
{
    Rig r;   // Neptune a un paramètre kVolume (10) ; le module aussi (même numéro)
    auto module = std::make_shared<FakeModule>();
    const auto target = insertTarget (kVolume);
    int resolved = 0;
    Config c;
    c.bindings = { macroToInsert (3, target) };
    r.engine.configure (c, r.set, [&] (const Target& t) -> ResolvedTarget { ++resolved; return t == target ? ResolvedTarget { module, &module->set } : ResolvedTarget {}; });
    CHECK (resolved == 1);
    const double neptuneBefore = r.set.get (kVolume);
    r.engine.macroChanged (3, 1.0, r.set);
    CHECK (module->set.getNormalized (kVolume) == Approx (1.0));
    CHECK (r.set.get (kVolume) == Approx (neptuneBefore));                 // le paramètre de Neptune n'a pas bougé
    std::vector<std::size_t> changed;
    CHECK (r.engine.drainChanges (changed) == 0);                          // rien à signaler à l'hôte pour un réglage d'insert
    r.engine.macroChanged (3, 0.0, r.set);
    CHECK (module->set.getNormalized (kVolume) == Approx (0.0));
}

TEST_CASE ("control: an unresolved insert target does nothing, and the module stays alive as long as the tables use it", "[control][insert]")
{
    Rig r;
    const auto target = insertTarget (kVolume);
    Config c;
    c.bindings = { macroToInsert (0, target) };
    // 1. aucun résolveur, ou résolveur qui ne trouve rien : l'assignation est gardée mais n'agit pas
    r.engine.configure (c, r.set);
    r.engine.macroChanged (0, 1.0, r.set);
    CHECK (r.set.get (kVolume) == Approx (0.0));
    r.engine.configure (c, r.set, [] (const Target&) { return ResolvedTarget {}; });
    r.engine.macroChanged (0, 1.0, r.set);
    CHECK (r.set.get (kVolume) == Approx (0.0));
    CHECK (r.engine.config().bindings.size() == 1);
    // 2. le module résolu est gardé en vie par les tables : le relâcher ailleurs ne laisse pas un pointeur pendant
    std::weak_ptr<FakeModule> watch;
    {
        auto module = std::make_shared<FakeModule>();
        watch = module;
        r.engine.configure (c, r.set, [&] (const Target&) { return ResolvedTarget { module, &module->set }; });
    }
    CHECK_FALSE (watch.expired());
    r.engine.macroChanged (0, 0.5, r.set);        // écrit dans le module encore en vie, sans accès invalide
    r.engine.configure (c, r.set);                // nouvelle table : l'ancienne est rendue au thread de message
    r.engine.macroChanged (0, 0.5, r.set);
    r.engine.collectGarbage();
    CHECK (watch.expired());                      // le module est libéré une fois les tables remplacées, sur ce thread
}

TEST_CASE ("control: insert targets round trip and a malformed one drops the whole assignment", "[control][insert]")
{
    Config c;
    c.bindings = { macroToInsert (2, insertTarget (7, 1, 5, 3, "equalizer")), macroToInsert (4, insertTarget (9, 3, 0, 7, "delay")) };
    c.bindings.push_back (cc (0, 7, kVolume));
    const auto back = configFromJson (toJson (c));
    REQUIRE (back.bindings.size() == 3);
    CHECK (back == c);
    CHECK (back.bindings[0].target.isInsert());
    CHECK_FALSE (back.bindings[2].target.isInsert());
    CHECK (back.bindings[2].target.isParam());
    // cibles mal formées : l'assignation est écartée (jamais lue comme un paramètre de Neptune du même numéro)
    const char* bad = R"({"bindings":[
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":{"strip":"nope","index":0,"slot":0,"type":"compressor"}}},
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":{"strip":"track","index":99,"slot":0,"type":"compressor"}}},
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":{"strip":"track","index":0,"slot":8,"type":"compressor"}}},
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":{"strip":"track","index":0,"slot":0,"type":""}}},
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":"oups"}},
        {"source":{"type":"macro","index":1},"target":{"param":10,"insert":{"strip":"master","index":0,"slot":2,"type":"limiter"}}}
    ]})";
    const auto s = configFromJson (bad);
    REQUIRE (s.bindings.size() == 1);
    CHECK (s.bindings[0].target.insertStrip == static_cast<int> (InsertStrip::Master));
    CHECK (s.bindings[0].target.insertType == "limiter");
}

TEST_CASE ("control: a CC drives an insert parameter, with its range and curve", "[control][insert]")
{
    Rig r;
    auto module = std::make_shared<FakeModule>();
    const auto target = insertTarget (kVolume);
    Binding b;
    b.source.kind = SourceKind::CC;
    b.source.channel = 0;
    b.source.number = 21;
    b.target = target;
    b.lo = 0.2;
    b.hi = 0.6;
    Config c;
    c.settings.ccEnabled = true;
    c.bindings = { b };
    r.engine.configure (c, r.set, [&] (const Target&) { return ResolvedTarget { module, &module->set }; });
    r.engine.controlChange (0, 21, 127, r.set);
    CHECK (module->set.getNormalized (kVolume) == Approx (0.6));
    r.engine.controlChange (0, 21, 0, r.set);
    CHECK (module->set.getNormalized (kVolume) == Approx (0.2));
}

TEST_CASE ("control: assignments to an insert that is gone are dropped, the others are kept", "[control][insert]")
{
    Config c;
    c.bindings = { macroToInsert (0, insertTarget (7, 0, 0, 1, "compressor")),     // gardée : le compresseur est toujours là
                   macroToInsert (1, insertTarget (7, 0, 0, 2, "compressor")),     // retirée : l'emplacement 2 est vide
                   macroToInsert (2, insertTarget (7, 0, 0, 1, "equalizer")),      // retirée : un autre type de module sur l'emplacement 1
                   cc (0, 7, kVolume) };                                            // un paramètre de Neptune : jamais touché
    int remaining = 0;
    const int dropped = dropMissingInsertTargets (c, [] (const Target& t) { return t.insertSlot == 1 && t.insertType == "compressor"; }, &remaining);
    CHECK (dropped == 2);
    CHECK (remaining == 1);
    REQUIRE (c.bindings.size() == 2);
    CHECK (c.bindings[0].source.number == 0);
    CHECK (c.bindings[1].target.isParam());
    // sans prédicat : toutes les cibles d'insert sont retirées, les autres restent
    Config d;
    d.bindings = { macroToInsert (0, insertTarget (7)), cc (0, 7, kVolume) };
    CHECK (dropMissingInsertTargets (d, {}) == 1);
    CHECK (d.bindings.size() == 1);
}
