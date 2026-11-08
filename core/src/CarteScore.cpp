#include "neptune/core/CarteScore.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>

namespace neptune::core::carte
{
using json = nlohmann::json;

namespace
{
const ScaleMode kModes[] = {
#include "ScaleCatalog.inc"
};
constexpr int kNumModes = static_cast<int> (sizeof (kModes) / sizeof (kModes[0]));

int pc (int v) { return ((v % 12) + 12) % 12; }

std::uint16_t rotate (std::uint16_t rel, int root)
{
    std::uint16_t r = 0;
    for (int s = 0; s < 12; ++s)
        if ((rel >> s) & 1u) r = static_cast<std::uint16_t> (r | (1u << pc (root + s)));
    return r;
}

template <typename T>
int placeSpan (std::vector<T>& v, T item, double minLength)
{
    if (! std::isfinite (item.start) || ! std::isfinite (item.length) || item.start < 0.0 || item.length <= 0.0) return -1;
    item.length = std::max (item.length, minLength);
    std::vector<T> out;
    for (const auto& o : v)
    {
        const double oe = o.start + o.length, ie = item.start + item.length;
        if (oe <= item.start || o.start >= ie) { out.push_back (o); continue; }   // pas de recouvrement
        if (o.start < item.start)                                                  // la partie avant le nouveau reste
        {
            T head = o;
            head.length = item.start - o.start;
            if (head.length >= minLength * 0.5) out.push_back (head);
        }
        if (oe > ie)                                                               // la partie après le nouveau reste
        {
            T tail = o;
            tail.start = ie;
            tail.length = oe - ie;
            if (tail.length >= minLength * 0.5) out.push_back (tail);
        }
    }
    out.push_back (item);
    std::stable_sort (out.begin(), out.end(), [] (const T& a, const T& b) { return a.start < b.start; });
    v = std::move (out);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].start == item.start) return static_cast<int> (i);
    return -1;
}
} // namespace

int scaleModeCount() { return kNumModes; }
const ScaleMode& scaleMode (int index) { return kModes[std::clamp (index, 0, kNumModes - 1)]; }
std::uint16_t scaleMask (int root, int mode) { return rotate (scaleMode (mode).rel, root); }
std::uint16_t scaleItemMask (const ScaleItem& s) { return s.rel != 0 ? rotate (static_cast<std::uint16_t> (s.rel | 1u), s.root) : scaleMask (s.root, s.mode); }

int placeScale (std::vector<ScaleItem>& v, ScaleItem item)
{
    if (item.rel != 0) { item.rel = static_cast<std::uint16_t> ((item.rel | 1u) & 0x0FFFu); item.mode = std::clamp (item.mode, 0, kNumModes - 1); if (item.name.size() > 40) item.name.resize (40); }
    else item.name.clear();
    if (item.root < 0 || item.root > 11 || item.mode < 0 || item.mode >= kNumModes) return -1;
    return placeSpan (v, item, kMinLength);
}
int placePattern (std::vector<PatternItem>& v, PatternItem item)
{
    if (item.slot < 1 || item.slot > 16) return -1;
    return placeSpan (v, item, kMinLength);
}
int placeKey (std::vector<KeyItem>& v, KeyItem item)
{
    if (! std::isfinite (item.start) || item.start < 0.0 || item.root < 0 || item.root > 11) return -1;
    for (auto& k : v)
        if (k.start == item.start) { k = item; return static_cast<int> (&k - v.data()); }
    v.push_back (item);
    std::stable_sort (v.begin(), v.end(), [] (const KeyItem& a, const KeyItem& b) { return a.start < b.start; });
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].start == item.start) return static_cast<int> (i);
    return -1;
}

int keyFifths (const KeyItem& k)
{
    const int majorPc = pc (k.root + (k.minor ? 3 : 0));   // tonique du majeur relatif
    const int f = (7 * majorPc) % 12;                     // quintes à monter, 0..11
    const bool black = majorPc == 1 || majorPc == 3 || majorPc == 6 || majorPc == 8 || majorPc == 10;
    if (! black) return f > 6 ? f - 12 : f;
    return k.sharp && f <= 7 ? f : f - 12;
}
KeyItem keyFromFifths (int fifths, bool minor)
{
    fifths = std::clamp (fifths, -7, 7);
    const int majorPc = pc (7 * fifths);   // 7 est son propre inverse modulo 12
    KeyItem k;
    k.root = pc (majorPc + (minor ? 9 : 0));
    k.minor = minor;
    k.sharp = fifths > 0;
    return k;
}

std::string scaleName (const ScaleItem& s, chords::Naming naming)
{
    chords::Chord c;
    c.root = s.root;
    c.sharp = s.sharp;
    c.rel = 0x091;   // majeur : le nom s'écrit sans suffixe
    const auto root = chords::formatChord (c, naming);
    if (s.rel != 0) return root + " " + (s.name.empty() ? std::string ("?") : s.name);   // gamme libre : le nom du fichier d'échange
    const auto& md = scaleMode (s.mode);
    return root + " " + (naming == chords::Naming::French ? md.nameFr : md.nameEn);
}
std::string keyName (const KeyItem& k, chords::Naming naming)
{
    chords::Chord c;
    c.root = k.root;
    c.sharp = k.sharp;
    c.rel = 0x091;   // majeur : le nom s'écrit sans suffixe
    const auto root = chords::formatChord (c, naming);
    if (naming == chords::Naming::French) return root + (k.minor ? " mineur" : " majeur");
    return root + (k.minor ? " minor" : " major");
}

std::vector<chordmap::Container> toContainers (const Score& score)
{
    auto v = chords::toContainers (score.chords);
    using chordmap::Container;
    using chordmap::Kind;
    for (const auto& s : score.scales)
    {
        Container c;
        c.kind = Kind::Scale;
        c.startBeats = s.start;
        c.lengthBeats = s.length;
        c.root = static_cast<std::uint8_t> (s.root);
        c.pitchMask = scaleItemMask (s);
        v.push_back (c);
    }
    for (const auto& p : score.patterns)
    {
        Container c;
        c.kind = Kind::Pattern;
        c.startBeats = p.start;
        c.lengthBeats = p.length;
        c.root = static_cast<std::uint8_t> (p.slot);
        v.push_back (c);
    }
    for (const auto& ms : score.measures)
    {
        Container c;
        c.kind = Kind::Measure;
        c.startBeats = ms.start;
        c.lengthBeats = ms.length;
        c.root = static_cast<std::uint8_t> (ms.num);
        c.bass = static_cast<std::uint8_t> (ms.den);
        v.push_back (c);
    }
    // tonalités : chaque marqueur dure jusqu'au suivant ; la tonalité globale couvre le début (ou tout le morceau sans marqueur)
    auto keyContainer = [] (const KeyItem& k, double start, double length)
    {
        Container c;
        c.kind = Kind::Key;
        c.startBeats = start;
        c.lengthBeats = length;
        c.root = static_cast<std::uint8_t> (k.root);
        c.pitchMask = scaleMask (k.root, k.minor ? kModeMinor : kModeMajor);
        return c;
    };
    if (score.hasGlobalKey)
    {
        const double end = score.keys.empty() ? kForever : score.keys.front().start;
        if (end > 0.0) v.push_back (keyContainer (score.globalKey, 0.0, end));
    }
    for (std::size_t i = 0; i < score.keys.size(); ++i)
    {
        const double end = i + 1 < score.keys.size() ? score.keys[i + 1].start : kForever;
        v.push_back (keyContainer (score.keys[i], score.keys[i].start, end - score.keys[i].start));
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------- enregistrement

std::string toJson (const Score& s)
{
    json j;
    j["carteVersion"] = 2;
    json ch = json::array();
    for (const auto& it : s.chords) ch.push_back ({ { "s", it.start }, { "l", it.length }, { "c", chords::formatChord (it.chord, chords::Naming::Letters) } });
    j["chords"] = ch;
    json sc = json::array();
    for (const auto& x : s.scales)
    {
        json o = { { "s", x.start }, { "l", x.length }, { "root", x.root }, { "mode", scaleMode (x.mode).id }, { "sharp", x.sharp } };
        if (x.rel != 0) { o["rel"] = x.rel; o["name"] = x.name; }   // gamme libre
        sc.push_back (std::move (o));
    }
    j["scales"] = sc;
    json ks = json::array();
    for (const auto& x : s.keys) ks.push_back ({ { "s", x.start }, { "root", x.root }, { "minor", x.minor }, { "sharp", x.sharp } });
    j["keys"] = ks;
    json ps = json::array();
    for (const auto& x : s.patterns) ps.push_back ({ { "s", x.start }, { "l", x.length }, { "slot", x.slot } });
    j["patterns"] = ps;
    json ms = json::array();
    for (const auto& x : s.measures) ms.push_back ({ { "s", x.start }, { "l", x.length }, { "n", x.num }, { "d", x.den }, { "src", x.source } });
    j["measures"] = ms;
    if (s.hasGlobalKey) j["globalKey"] = { { "root", s.globalKey.root }, { "minor", s.globalKey.minor }, { "sharp", s.globalKey.sharp } };
    return j.dump();
}

namespace
{
int modeIndexOf (const std::string& id)
{
    for (int i = 0; i < kNumModes; ++i) if (id == kModes[i].id) return i;
    return -1;
}
}

Score fromJson (const std::string& text)
{
    Score s;
    const auto j = json::parse (text, nullptr, false);
    if (j.is_discarded() || ! j.is_object())
    {
        s.chords = chords::fromText (text);   // Carte v1 : le texte des accords seuls
        return s;
    }
    auto num = [] (const json& o, const char* k, double d) { return o.contains (k) && o[k].is_number() ? o[k].get<double>() : d; };
    if (j.contains ("chords") && j["chords"].is_array())
        for (const auto& o : j["chords"])
        {
            chords::Item it;
            it.start = num (o, "s", -1.0);
            it.length = num (o, "l", 0.0);
            if (it.start >= 0.0 && it.length > 0.0 && o.contains ("c") && o["c"].is_string() && chords::parseChord (o["c"].get<std::string>(), it.chord)) s.chords.push_back (it);
        }
    // tri, puis recouvrements coupés en une passe (un état abîmé ne doit pas coûter un temps quadratique)
    std::stable_sort (s.chords.begin(), s.chords.end(), [] (const chords::Item& x, const chords::Item& y) { return x.start < y.start; });
    {
        std::vector<chords::Item> clean;
        clean.reserve (s.chords.size());
        for (const auto& it : s.chords)
        {
            if (! clean.empty() && clean.back().start + clean.back().length > it.start)
            {
                clean.back().length = it.start - clean.back().start;
                if (clean.back().length < chords::kMinLength) clean.pop_back();
            }
            clean.push_back (it);
        }
        s.chords = std::move (clean);
    }
    if (j.contains ("scales") && j["scales"].is_array())
        for (const auto& o : j["scales"])
        {
            ScaleItem x;
            x.start = num (o, "s", -1.0);
            x.length = num (o, "l", 0.0);
            x.root = static_cast<int> (num (o, "root", 0));
            x.sharp = o.value ("sharp", false);
            x.mode = o.contains ("mode") && o["mode"].is_string() ? modeIndexOf (o["mode"].get<std::string>()) : -1;
            if (o.contains ("rel") && o["rel"].is_number_integer())
            {
                x.rel = static_cast<std::uint16_t> (std::clamp (o["rel"].get<int>(), 0, 0x0FFF));
                if (o.contains ("name") && o["name"].is_string()) x.name = o["name"].get<std::string>();
                if (x.mode < 0) x.mode = 0;
            }
            placeScale (s.scales, x);
        }
    if (j.contains ("keys") && j["keys"].is_array())
        for (const auto& o : j["keys"])
        {
            KeyItem x;
            x.start = num (o, "s", -1.0);
            x.root = static_cast<int> (num (o, "root", 0));
            x.minor = o.value ("minor", false);
            x.sharp = o.value ("sharp", false);
            placeKey (s.keys, x);
        }
    if (j.contains ("patterns") && j["patterns"].is_array())
        for (const auto& o : j["patterns"])
        {
            PatternItem x;
            x.start = num (o, "s", -1.0);
            x.length = num (o, "l", 0.0);
            x.slot = static_cast<int> (num (o, "slot", 0));
            placePattern (s.patterns, x);
        }
    if (j.contains ("measures") && j["measures"].is_array())
        for (const auto& o : j["measures"])
        {
            MeasureItem x;
            x.start = num (o, "s", -1.0);
            x.length = num (o, "l", 0.0);
            x.num = static_cast<int> (num (o, "n", 4));
            x.den = static_cast<int> (num (o, "d", 4));
            x.source = static_cast<int> (num (o, "src", 0));
            if (x.start >= 0.0 && x.length > 0.0 && x.num >= 1 && x.num <= 64 && x.den >= 1 && x.den <= 64) s.measures.push_back (x);
        }
    std::stable_sort (s.measures.begin(), s.measures.end(), [] (const MeasureItem& a, const MeasureItem& b) { return a.start < b.start; });
    if (j.contains ("globalKey") && j["globalKey"].is_object())
    {
        const auto& g = j["globalKey"];
        s.hasGlobalKey = true;
        s.globalKey.root = std::clamp (static_cast<int> (num (g, "root", 0)), 0, 11);
        s.globalKey.minor = g.value ("minor", false);
        s.globalKey.sharp = g.value ("sharp", false);
    }
    return s;
}

Score demoScore (double startBeats)
{
    Score s;
    const char* names[8] = { "C", "Am7", "F", "G7", "Dm7", "Am7/D", "C", "Cmaj7" };
    double t = startBeats;
    for (int m = 0; m < 8; ++m)
    {
        const double len = m == 7 ? 3.0 : 4.0;   // la dernière mesure est un 3/4
        MeasureItem ms;
        ms.start = t;
        ms.length = len;
        ms.num = m == 7 ? 3 : 4;
        ms.den = 4;
        ms.source = m;
        s.measures.push_back (ms);
        chords::Item it;
        it.start = t;
        it.length = len;
        chords::parseChord (names[m], it.chord);
        s.chords.push_back (it);
        t += len;
    }
    const double b = startBeats;
    placeScale (s.scales, { b + 0.0, 16.0, 0, kModeMajor, false });       // do majeur sur les quatre premiers accords
    placeScale (s.scales, { b + 16.0, 8.0, 2, 4, false });                // ré dorien sur Dm7 et Am7/D
    placeScale (s.scales, { b + 24.0, 7.0, 0, kModeMajor, false });       // retour en do majeur
    s.hasGlobalKey = true;
    s.globalKey.root = 0;
    placeKey (s.keys, { b + 16.0, 5, false, false });                     // marqueur : fa majeur à partir de la mesure 5 (la gamme de Dm7 en dorien en est un mode)
    placePattern (s.patterns, { b + 0.0, 8.0, 12 });                      // mesures 1-2 : Essai 1 (notes, vélocité et durée par coup)
    placePattern (s.patterns, { b + 8.0, 8.0, 13 });                      // mesures 3-4 : Essai 2 (accord, variante tirée au sort)
    placePattern (s.patterns, { b + 16.0, 8.0, 14 });                     // mesures 5-6 : Essai 3 (ballade)
    placePattern (s.patterns, { b + 24.0, 7.0, 15 });                     // mesures 7-8 : Essai 4 (deux mesures)
    return s;
}

// ---------------------------------------------------------------------------------------------------- import

namespace
{
bool readFrac (const json& j, double& beats)
{
    if (! j.is_array() || j.size() != 2 || ! j[0].is_number_integer() || ! j[1].is_number_integer()) return false;
    const auto n = j[0].get<long long>(), d = j[1].get<long long>();
    if (d <= 0 || n < 0) return false;
    beats = 4.0 * static_cast<double>(n) / static_cast<double>(d);   // 1 ronde = 4 noires
    return true;
}

// « C », « F# », « Bbb », « C## » : classe de hauteur et écriture avec dièse.
bool readNote (const std::string& t, int& pcOut, bool& sharp)
{
    if (t.empty() || t[0] < 'A' || t[0] > 'G') return false;
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    int v = base[t[0] - 'A'];
    sharp = false;
    for (std::size_t i = 1; i < t.size(); ++i)
    {
        if (t[i] == '#') { ++v; sharp = true; }
        else if (t[i] == 'b') --v;
        else return false;
    }
    pcOut = pc (v);
    return true;
}

std::uint16_t relFromIntervals (const json& arr)
{
    std::uint16_t r = 0;
    if (! arr.is_array()) return 0;
    for (const auto& x : arr)
        if (x.is_number_integer() && x.get<int>() >= 0) r = static_cast<std::uint16_t> (r | (1u << (x.get<int>() % 12)));
    return r;
}
}

namespace
{
// Le nom d'une note : « C », « F# », « Bb » selon l'écriture (dièse ou bémol).
std::string noteText (int pcv, bool sharp)
{
    static const char* const withSharps[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* const withFlats[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    return (sharp ? withSharps : withFlats)[pc (pcv)];
}

// Identifiant du catalogue de gammes de Scorsaire d'un mode de Carte (le chromatique n'y est pas : il garde l'identifiant de Carte).
const char* scorsaireIdOf (int mode) { return kModes[mode].scorsaireId[0] != '\0' ? kModes[mode].scorsaireId : kModes[mode].id; }

int modeOfScorsaireId (const std::string& id)
{
    for (int i = 0; i < kNumModes; ++i) if (id == scorsaireIdOf (i)) return i;
    return -1;
}

json fractionOf (double beats)
{
    // 960 pulsations par noire : 3840 par ronde
    long long n = std::llround (std::max (0.0, beats) / 4.0 * 3840.0), d = 3840;
    long long a = n, b = d;
    while (b != 0) { const long long t = a % b; a = b; b = t; }
    if (a > 1) { n /= a; d /= a; }
    return json::array ({ n, d });
}

// Les intervalles d'un accord s'écrivent sur deux octaves (une neuvième = 14), comme chez Scorsaire.
json intervalsOf (std::uint32_t rel, int bits = 12)
{
    json a = json::array();
    for (int i = 0; i < bits; ++i) if ((rel >> i) & 1u) a.push_back (i);
    return a;
}

// Intervalles d'une qualité d'accord du fichier : de 0 à 23 demi-tons ; au-delà, ramenés dans l'octave.
std::uint32_t chordIntervalsFrom (const json& arr)
{
    std::uint32_t r = 0;
    if (! arr.is_array()) return 0;
    for (const auto& x : arr)
        if (x.is_number_integer() && x.get<int>() >= 0) r |= 1u << (x.get<int>() < kIntervalBits ? x.get<int>() : x.get<int>() % 12);
    return r;
}

// Une tonalité : `fifths` (de −7 à 7) et/ou `root` + `mode`. Si les deux sont là et d'accord, l'écriture de `root` est gardée ; sinon `fifths` fait foi
// (avec le mode du fichier s'il y en a un, majeur par défaut).
bool readKey (const json& o, KeyItem& k)
{
    int rootPc = 0;
    bool sharp = false;
    const auto mode = o.contains ("mode") && o["mode"].is_string() ? o["mode"].get<std::string>() : std::string();
    const bool hasMode = mode == "major" || mode == "minor";
    const bool hasRoot = o.contains ("root") && o["root"].is_string() && readNote (o["root"].get<std::string>(), rootPc, sharp) && hasMode;
    const bool hasFifths = o.contains ("fifths") && o["fifths"].is_number_integer() && o["fifths"].get<int>() >= -7 && o["fifths"].get<int>() <= 7;
    if (hasRoot)
    {
        k.root = rootPc;
        k.minor = mode == "minor";
        k.sharp = sharp;
        if (! hasFifths || keyFifths (k) == o["fifths"].get<int>()) return true;
    }
    if (! hasFifths) return false;
    k = keyFromFifths (o["fifths"].get<int>(), mode == "minor");
    return true;
}

// Les voies propres à `flibuste-carte` : gammes, tonalités, appels de motifs.
void readCarteLanes (const json& j, double startBeats, const std::vector<std::string>& patternNames, ImportResult& r)
{
    auto arrayOf = [&j] (const char* key) -> const json* { return j.contains (key) && j[key].is_array() ? &j[key] : nullptr; };
    if (const auto* sc = arrayOf ("scales"))
    {
        r.hasScales = true;
        if (static_cast<int> (sc->size()) > kMaxImportScales) { r.error = "Le fichier a " + std::to_string (sc->size()) + " gammes : Carte en accepte au plus " + std::to_string (kMaxImportScales) + ". Rien n'a été importé."; return; }
        for (const auto& o : *sc)
        {
            double at = 0.0, dur = 0.0;
            int rootPc = 0;
            bool sharp = false;
            if (! o.is_object() || ! o.contains ("at") || ! o.contains ("dur") || ! readFrac (o["at"], at) || ! readFrac (o["dur"], dur) || dur <= 0.0
                || ! o.contains ("root") || ! o["root"].is_string() || ! readNote (o["root"].get<std::string>(), rootPc, sharp)) { ++r.skipped; continue; }
            ScaleItem x;
            x.start = startBeats + at;
            x.length = dur;
            x.root = rootPc;
            x.sharp = sharp;
            const std::string id = o.contains ("scale") && o["scale"].is_string() ? o["scale"].get<std::string>() : std::string();
            const int known = modeOfScorsaireId (id);
            const std::uint16_t mask = o.contains ("intervals") ? relFromIntervals (o["intervals"]) : std::uint16_t (0);
            if (known >= 0 && (mask == 0 || mask == kModes[known].rel)) x.mode = known;                  // une gamme de la liste, identique à celle du fichier
            else if (mask != 0)                                                                            // gamme libre : les intervalles du fichier font foi
            {
                x.rel = static_cast<std::uint16_t> (mask | 1u);
                x.name = o.contains ("name") && o["name"].is_string() ? o["name"].get<std::string>() : id;
            }
            else { ++r.unknownScales; continue; }                                                          // ni intervalles, ni identifiant connu
            if (placeScale (r.score.scales, x) < 0) ++r.skipped;
        }
    }
    if (const auto* ks = arrayOf ("keys"))
    {
        r.hasKeys = true;
        if (static_cast<int> (ks->size()) > kMaxImportKeys) { r.error = "Le fichier a " + std::to_string (ks->size()) + " tonalités : Carte en accepte au plus " + std::to_string (kMaxImportKeys) + ". Rien n'a été importé."; return; }
        for (const auto& o : *ks)
        {
            double at = 0.0;
            KeyItem k;
            if (! o.is_object() || ! o.contains ("at") || ! readFrac (o["at"], at) || ! readKey (o, k)) { ++r.skipped; continue; }
            k.start = startBeats + at;
            if (placeKey (r.score.keys, k) < 0) ++r.skipped;
        }
    }
    if (j.contains ("globalKey") && j["globalKey"].is_object())
    {
        KeyItem g;
        if (readKey (j["globalKey"], g))
        {
            r.score.hasGlobalKey = true;
            r.score.globalKey = g;
        }
    }
    if (const auto* ps = arrayOf ("patterns"))
    {
        r.hasPatterns = true;
        if (static_cast<int> (ps->size()) > kMaxImportPatterns) { r.error = "Le fichier a " + std::to_string (ps->size()) + " appels de motifs : Carte en accepte au plus " + std::to_string (kMaxImportPatterns) + ". Rien n'a été importé."; return; }
        for (const auto& o : *ps)
        {
            double at = 0.0, dur = 0.0;
            if (! o.is_object() || ! o.contains ("at") || ! o.contains ("dur") || ! readFrac (o["at"], at) || ! readFrac (o["dur"], dur) || dur <= 0.0
                || ! o.contains ("slot") || ! o["slot"].is_number_integer()) { ++r.skipped; continue; }
            PatternItem x;
            x.start = startBeats + at;
            x.length = dur;
            x.slot = o["slot"].get<int>();
            if (placePattern (r.score.patterns, x) < 0) { ++r.skipped; continue; }
            const std::string name = o.contains ("name") && o["name"].is_string() ? o["name"].get<std::string>() : std::string();
            const std::string mine = x.slot >= 1 && static_cast<std::size_t> (x.slot) <= patternNames.size() ? patternNames[static_cast<std::size_t> (x.slot) - 1] : std::string();
            if (! name.empty() && ! mine.empty() && name != mine)
            {
                const std::string note = "L'emplacement " + std::to_string (x.slot) + " s'appelle « " + name + " » dans le fichier et « " + mine + " » ici.";
                if (std::find (r.notes.begin(), r.notes.end(), note) == r.notes.end()) r.notes.push_back (note);
            }
        }
    }
}

ImportResult importCommon (const std::string& text, double startBeats, bool carteFormat, const std::vector<std::string>& patternNames)
{
    ImportResult r;
    const auto j = json::parse (text, nullptr, false);
    if (j.is_discarded() || ! j.is_object()) { r.error = "Fichier illisible (ce n'est pas du JSON)."; return r; }
    const auto fmt = j.contains ("format") && j["format"].is_string() ? j["format"].get<std::string>() : std::string();
    const bool isCarte = fmt == "flibuste-carte";
    if (fmt != "flibuste-accords" && ! (carteFormat && isCarte))
    {
        r.error = carteFormat ? "Ce fichier n'est ni une carte (flibuste-carte) ni une piste d'accords (flibuste-accords) : format « " + fmt + " »."
                              : "Ce fichier n'est pas une piste d'accords de Scorsaire (format « " + fmt + " »).";
        return r;
    }
    const auto version = j.value ("version", 0);
    if (version < 1 || version > 1) { r.error = "Version " + std::to_string (version) + " du format non prise en charge : mettez à jour la suite."; return r; }
    r.title = j.value ("title", std::string());
    // un fichier d'accords ne porte que les accords et les mesures, qu'il remplace toujours ; une carte ne remplace que les voies qu'elle contient
    r.hasChords = ! isCarte || (j.contains ("chords") && j["chords"].is_array());
    r.hasMeasures = ! isCarte || (j.contains ("measures") && j["measures"].is_array());
    const json empty = json::array();
    const auto& ms = j.contains ("measures") && j["measures"].is_array() ? j["measures"] : empty;
    const auto& cs = j.contains ("chords") && j["chords"].is_array() ? j["chords"] : empty;
    if (static_cast<int> (ms.size()) > kMaxImportMeasures)
    {
        r.error = "Le morceau a " + std::to_string (ms.size()) + " mesures : Carte en accepte au plus " + std::to_string (kMaxImportMeasures) + ". Rien n'a été importé.";
        return r;
    }
    if (static_cast<int> (cs.size()) > kMaxImportChords)
    {
        r.error = "Le morceau a " + std::to_string (cs.size()) + " accords : Carte en accepte au plus " + std::to_string (kMaxImportChords) + ". Rien n'a été importé.";
        return r;
    }
    // dictionnaire du fichier : il fait foi pour ce morceau
    std::map<std::string, std::uint32_t> qualities;
    if (j.contains ("qualities") && j["qualities"].is_object())
        for (auto it = j["qualities"].begin(); it != j["qualities"].end(); ++it) qualities[it.key()] = chordIntervalsFrom (it.value());

    for (const auto& o : ms)
    {
        MeasureItem x;
        double at = 0.0, dur = 0.0;
        if (! o.is_object() || ! o.contains ("at") || ! o.contains ("dur") || ! readFrac (o["at"], at) || ! readFrac (o["dur"], dur) || dur <= 0.0) continue;
        x.start = startBeats + at;
        x.length = dur;
        if (o.contains ("ts") && o["ts"].is_array() && o["ts"].size() == 2 && o["ts"][0].is_number_integer() && o["ts"][1].is_number_integer())
        {
            x.num = std::clamp (o["ts"][0].get<int>(), 1, 64);
            x.den = std::clamp (o["ts"][1].get<int>(), 1, 64);
        }
        x.source = o.value ("source", 0);
        r.score.measures.push_back (x);
    }
    for (const auto& o : cs)
    {
        double at = 0.0, dur = 0.0;
        if (! o.is_object() || ! o.contains ("at") || ! o.contains ("dur") || ! readFrac (o["at"], at) || ! readFrac (o["dur"], dur) || dur <= 0.0) continue;
        if (o.value ("nc", false)) { ++r.noChord; continue; }   // « pas d'accord » : un trou
        chords::Item it;
        it.start = startBeats + at;
        it.length = dur;
        int rootPc = 0;
        bool sharp = false;
        if (! o.contains ("root") || ! o["root"].is_string() || ! readNote (o["root"].get<std::string>(), rootPc, sharp)) continue;
        it.chord.root = rootPc;
        it.chord.sharp = sharp;
        const auto q = o.value ("quality", std::string());
        const auto found = qualities.find (q);
        if (found != qualities.end() && found->second != 0) it.chord.rel = found->second;
        else
        {
            chords::Chord parsed;
            if (chords::parseChord (o["root"].get<std::string>() + q, parsed)) it.chord.rel = parsed.rel;
            else { it.chord.rel = 1; ++r.unknownQualities; }   // qualité inconnue : la fondamentale seule
        }
        it.chord.rel |= 1u;
        if (o.contains ("bass") && o["bass"].is_string())
        {
            int b = 0;
            bool bs = false;
            if (readNote (o["bass"].get<std::string>(), b, bs)) { it.chord.bass = b; it.chord.sharp = it.chord.sharp || bs; }
        }
        r.score.chords.push_back (it);
    }
    std::stable_sort (r.score.chords.begin(), r.score.chords.end(), [] (const chords::Item& a, const chords::Item& b) { return a.start < b.start; });
    std::stable_sort (r.score.measures.begin(), r.score.measures.end(), [] (const MeasureItem& a, const MeasureItem& b) { return a.start < b.start; });
    if (isCarte) readCarteLanes (j, startBeats, patternNames, r);
    r.ok = ! r.error.empty() ? false : true;
    return r;
}

} // namespace

ImportResult importChordFile (const std::string& text, double startBeats) { return importCommon (text, startBeats, false, {}); }
ImportResult importCarteFile (const std::string& text, double startBeats, const std::vector<std::string>& patternNames) { return importCommon (text, startBeats, true, patternNames); }

std::string exportCarteFile (const Score& s, const std::string& title, double startBeats, const std::vector<std::string>& patternNames)
{
    json j;
    j["format"] = "flibuste-carte";
    j["version"] = 1;
    j["from"] = "Neptune Carte";
    j["unrolled"] = true;
    j["title"] = title;
    auto rel = [startBeats] (double beats) { return fractionOf (beats - startBeats); };
    // mesures et chiffrages
    json ms = json::array(), ts = json::array();
    int lastNum = 0, lastDen = 0, index = 0;
    for (const auto& m : s.measures)
    {
        ms.push_back ({ { "at", rel (m.start) }, { "dur", fractionOf (m.length) }, { "ts", json::array ({ m.num, m.den }) }, { "source", m.source } });
        if (m.num != lastNum || m.den != lastDen) ts.push_back ({ { "fromMeasure", index }, { "ts", json::array ({ m.num, m.den }) } });
        lastNum = m.num;
        lastDen = m.den;
        ++index;
    }
    j["timeSignatures"] = ts;
    j["measures"] = ms;
    // accords : l'identifiant de qualité est l'écriture affichée (« m7 »), son dictionnaire est dans le fichier ; le majeur est « »
    json qualities = json::object(), cs = json::array();
    for (const auto& it : s.chords)
    {
        auto bare = it.chord;
        bare.bass = -1;
        const auto root = noteText (it.chord.root, it.chord.sharp);
        const auto full = chords::formatChord (bare, chords::Naming::Letters);
        const auto suffix = full.compare (0, root.size(), root) == 0 ? full.substr (root.size()) : std::string();
        if (! qualities.contains (suffix)) qualities[suffix] = intervalsOf (it.chord.rel, kIntervalBits);
        json o = { { "at", rel (it.start) }, { "dur", fractionOf (it.length) }, { "root", root }, { "quality", suffix } };
        o["bass"] = it.chord.bass >= 0 ? json (noteText (it.chord.bass, it.chord.sharp)) : json (nullptr);
        cs.push_back (std::move (o));
    }
    j["qualities"] = qualities;
    j["chords"] = cs;
    // gammes
    json sc = json::array();
    for (const auto& x : s.scales)
    {
        const std::uint16_t notes = x.rel != 0 ? static_cast<std::uint16_t> (x.rel | 1u) : scaleMode (x.mode).rel;
        json o = { { "at", rel (x.start) }, { "dur", fractionOf (x.length) }, { "root", noteText (x.root, x.sharp) }, { "intervals", intervalsOf (notes) } };
        if (x.rel == 0) { o["scale"] = scorsaireIdOf (x.mode); o["name"] = scaleMode (x.mode).nameFr; }
        else if (! x.name.empty()) o["name"] = x.name;
        sc.push_back (std::move (o));
    }
    j["scales"] = sc;
    // tonalités
    json ks = json::array();
    for (const auto& x : s.keys) ks.push_back ({ { "at", rel (x.start) }, { "root", noteText (x.root, x.sharp) }, { "mode", x.minor ? "minor" : "major" }, { "fifths", keyFifths (x) } });
    j["keys"] = ks;
    if (s.hasGlobalKey) j["globalKey"] = { { "root", noteText (s.globalKey.root, s.globalKey.sharp) }, { "mode", s.globalKey.minor ? "minor" : "major" }, { "fifths", keyFifths (s.globalKey) } };
    // appels de motifs
    json ps = json::array();
    for (const auto& x : s.patterns)
    {
        json o = { { "at", rel (x.start) }, { "dur", fractionOf (x.length) }, { "slot", x.slot } };
        if (x.slot >= 1 && static_cast<std::size_t> (x.slot) <= patternNames.size() && ! patternNames[static_cast<std::size_t> (x.slot) - 1].empty()) o["name"] = patternNames[static_cast<std::size_t> (x.slot) - 1];
        ps.push_back (std::move (o));
    }
    j["patterns"] = ps;
    return j.dump (2);
}
} // namespace neptune::core::carte
