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
constexpr std::uint16_t m (std::initializer_list<int> v)
{
    std::uint16_t r = 0;
    for (int x : v) r = static_cast<std::uint16_t> (r | (1u << x));
    return r;
}
const ScaleMode kModes[] = {
    { "major", "majeure", "major", m ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "minor", "mineure naturelle", "natural minor", m ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "harmonic-minor", "mineure harmonique", "harmonic minor", m ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "melodic-minor", "mineure mélodique", "melodic minor", m ({ 0, 2, 3, 5, 7, 9, 11 }) },
    { "dorian", "dorienne", "dorian", m ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "phrygian", "phrygienne", "phrygian", m ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "lydian", "lydienne", "lydian", m ({ 0, 2, 4, 6, 7, 9, 11 }) },
    { "mixolydian", "mixolydienne", "mixolydian", m ({ 0, 2, 4, 5, 7, 9, 10 }) },
    { "locrian", "locrienne", "locrian", m ({ 0, 1, 3, 5, 6, 8, 10 }) },
    { "pentatonic-major", "pentatonique majeure", "major pentatonic", m ({ 0, 2, 4, 7, 9 }) },
    { "pentatonic-minor", "pentatonique mineure", "minor pentatonic", m ({ 0, 3, 5, 7, 10 }) },
    { "blues", "blues", "blues", m ({ 0, 3, 5, 6, 7, 10 }) },
    { "whole-tone", "par tons", "whole tone", m ({ 0, 2, 4, 6, 8, 10 }) },
    { "diminished-hw", "diminuée (demi-ton, ton)", "diminished (half-whole)", m ({ 0, 1, 3, 4, 6, 7, 9, 10 }) },
    { "diminished-wh", "diminuée (ton, demi-ton)", "diminished (whole-half)", m ({ 0, 2, 3, 5, 6, 8, 9, 11 }) },
    { "altered", "altérée", "altered", m ({ 0, 1, 3, 4, 6, 8, 10 }) },
    { "lydian-dominant", "lydienne dominante", "lydian dominant", m ({ 0, 2, 4, 6, 7, 9, 10 }) },
    { "chromatic", "chromatique", "chromatic", 0x0FFF },
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

int placeScale (std::vector<ScaleItem>& v, ScaleItem item)
{
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

std::string scaleName (const ScaleItem& s, chords::Naming naming)
{
    chords::Chord c;
    c.root = s.root;
    c.sharp = s.sharp;
    c.rel = 1;
    const auto root = chords::formatChord (c, naming);
    const auto& md = scaleMode (s.mode);
    return root + " " + (naming == chords::Naming::French ? md.nameFr : md.nameEn);
}
std::string keyName (const KeyItem& k, chords::Naming naming)
{
    chords::Chord c;
    c.root = k.root;
    c.sharp = k.sharp;
    c.rel = 1;
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
        c.pitchMask = scaleMask (s.root, s.mode);
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
    for (const auto& x : s.scales) sc.push_back ({ { "s", x.start }, { "l", x.length }, { "root", x.root }, { "mode", scaleMode (x.mode).id }, { "sharp", x.sharp } });
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

ImportResult importChordFile (const std::string& text, double startBeats)
{
    ImportResult r;
    const auto j = json::parse (text, nullptr, false);
    if (j.is_discarded() || ! j.is_object()) { r.error = "Fichier illisible (ce n'est pas du JSON)."; return r; }
    const auto fmt = j.contains ("format") && j["format"].is_string() ? j["format"].get<std::string>() : std::string();
    if (fmt != "flibuste-accords") { r.error = "Ce fichier n'est pas une piste d'accords de Scorsaire (format « " + fmt + " »)."; return r; }
    const auto version = j.value ("version", 0);
    if (version < 1 || version > 1) { r.error = "Version " + std::to_string (version) + " du format non prise en charge : mettez à jour la suite."; return r; }
    r.title = j.value ("title", std::string());
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
    std::map<std::string, std::uint16_t> qualities;
    if (j.contains ("qualities") && j["qualities"].is_object())
        for (auto it = j["qualities"].begin(); it != j["qualities"].end(); ++it) qualities[it.key()] = relFromIntervals (it.value());

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
        it.chord.rel = static_cast<std::uint16_t> (it.chord.rel | 1u);
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
    r.ok = true;
    return r;
}
} // namespace neptune::core::carte
