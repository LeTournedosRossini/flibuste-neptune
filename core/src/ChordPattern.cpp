#include "neptune/core/ChordPattern.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <nlohmann/json.hpp>

namespace neptune::core::pattern
{
using json = nlohmann::json;

Frac makeFrac (std::int64_t n, std::int64_t d)
{
    if (d < 0) { n = -n; d = -d; }
    if (d == 0) return { 0, 1 };
    const auto g = std::gcd (n < 0 ? -n : n, d);
    return g > 1 ? Frac { n / g, d / g } : Frac { n, d };
}
int compare (const Frac& a, const Frac& b)
{
    const auto l = a.n * b.d, r = b.n * a.d;
    return l < r ? -1 : l > r ? 1 : 0;
}
Frac operator+ (const Frac& a, const Frac& b) { return makeFrac (a.n * b.d + b.n * a.d, a.d * b.d); }
Frac operator- (const Frac& a, const Frac& b) { return makeFrac (a.n * b.d - b.n * a.d, a.d * b.d); }

Frac Pattern::barLength() const { return makeFrac (length.n, length.d * bars); }

// ================================================================================================ parts

namespace
{
const int kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };
int majorInterval (int n) { return kMajor[(n - 1) % 7] + 12 * ((n - 1) / 7); }
}

bool parsePart (const std::string& textIn, Part& out)
{
    std::string t;
    for (char c : textIn) if (c != ' ') t += c;
    Part p;
    p.text = t;
    if (t == "B") { p.kind = Part::Kind::Bass; out = p; return true; }
    if (t == "C") { p.kind = Part::Kind::Chord; out = p; return true; }
    size_t i = 0;
    if (i < t.size() && t[i] == 'L') { p.leftHand = true; ++i; }
    int acc = 0;
    bool sgn = false;
    if (i < t.size() && (t[i] == '#' || t[i] == 'b'))
    {
        const char c = t[i];
        int k = 0;
        while (i < t.size() && t[i] == c && k < 2) { ++i; ++k; }
        acc = c == '#' ? k : -k;
        sgn = true;
    }
    size_t j = i;
    while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
    if (j == i || j - i > 2) return false;
    const int n = std::stoi (t.substr (i, j - i));
    if (n < 1 || n > 15) return false;
    int oct = 0;
    if (j < t.size())
    {
        if (j + 2 != t.size() || (t[j] != '+' && t[j] != '-') || t[j + 1] < '0' || t[j + 1] > '9') return false;
        oct = (t[j + 1] - '0') * (t[j] == '-' ? -1 : 1);
    }
    p.kind = Part::Kind::Degree;
    p.degree = n;
    p.accidental = acc;
    p.signedDegree = sgn;
    p.octave = oct;
    out = p;
    return true;
}

// ================================================================================================ json

namespace
{
bool readFrac (const json& j, Frac& out)
{
    if (! j.is_array() || j.size() != 2 || ! j[0].is_number_integer() || ! j[1].is_number_integer()) return false;
    const auto n = j[0].get<std::int64_t>(), d = j[1].get<std::int64_t>();
    if (d <= 0 || n < 0) return false;
    out = makeFrac (n, d);
    return true;
}
json fracJson (const Frac& f) { return json::array ({ f.n, f.d }); }

std::string extrasOf (const json& obj, std::initializer_list<const char*> known)
{
    json extra = json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        bool isKnown = false;
        for (auto* k : known) if (it.key() == k) isKnown = true;
        if (! isKnown) extra[it.key()] = it.value();
    }
    return extra.empty() ? std::string() : extra.dump();
}

bool readHit (const json& j, const Frac& limit, Hit& out)
{
    if (! j.is_object()) return false;
    Hit h;
    if (! j.contains ("at") || ! j.contains ("dur") || ! readFrac (j["at"], h.at) || ! readFrac (j["dur"], h.dur)) return false;
    if (h.dur.n == 0 || compare (h.at, limit) >= 0) return false;
    if (j.contains ("parts") && j["parts"].is_array())
        for (const auto& s : j["parts"])
        {
            Part p;
            if (s.is_string() && parsePart (s.get<std::string>(), p)) h.parts.push_back (p);
        }
    if (h.parts.empty()) return false;
    h.accent = j.value ("accent", false);
    if (j.contains ("vel") && j["vel"].is_number_integer())
    {
        const auto v = j["vel"].get<long long>();
        if (v >= 1 && v <= 127) h.vel = (int) v;
    }
    if (j.contains ("gate") && j["gate"].is_number())
    {
        const double g = j["gate"].get<double>();
        if (g >= 0.05 && g <= 1.0) h.gate = g;
    }
    h.extraJson = extrasOf (j, { "at", "dur", "parts", "accent", "vel", "gate" });
    out = h;
    return true;
}

bool readPattern (const json& j, Pattern& out)
{
    if (! j.is_object()) return false;
    Pattern p;
    p.id = j.value ("id", std::string());
    if (j.contains ("name") && j["name"].is_string()) p.name = j["name"].get<std::string>();
    if (j.contains ("length"))
    {
        Frac f;
        if (readFrac (j["length"], f) && f.n > 0) p.length = f;
    }
    if (j.contains ("bars") && j["bars"].is_number_integer())
    {
        const auto b = j["bars"].get<long long>();
        if (b >= 1 && b <= kMaxBars) p.bars = (int) b;
    }
    if (j.contains ("hits") && j["hits"].is_array())
        for (const auto& hj : j["hits"])
        {
            Hit h;
            if (p.hits.size() < (size_t) kMaxHits && readHit (hj, p.length, h)) p.hits.push_back (h);
        }
    if (p.hits.empty()) return false;
    std::stable_sort (p.hits.begin(), p.hits.end(), [] (const Hit& a, const Hit& b) { return compare (a.at, b.at) < 0; });
    p.extraJson = extrasOf (j, { "id", "name", "length", "bars", "hits" });
    out = p;
    return true;
}

json patternJson (const Pattern& p)
{
    json j = p.extraJson.empty() ? json::object() : json::parse (p.extraJson, nullptr, false);
    if (! j.is_object()) j = json::object();
    j["id"] = p.id;
    if (! p.name.empty()) j["name"] = p.name;
    j["length"] = fracJson (p.length);
    if (p.bars > 1) j["bars"] = p.bars;
    json hits = json::array();
    for (const auto& h : p.hits)
    {
        json hj = h.extraJson.empty() ? json::object() : json::parse (h.extraJson, nullptr, false);
        if (! hj.is_object()) hj = json::object();
        hj["at"] = fracJson (h.at);
        hj["dur"] = fracJson (h.dur);
        json parts = json::array();
        for (const auto& pt : h.parts) parts.push_back (pt.text);
        hj["parts"] = parts;
        if (h.accent) hj["accent"] = true;
        if (h.vel > 0) hj["vel"] = h.vel;
        if (h.gate > 0.0) hj["gate"] = h.gate;
        hits.push_back (hj);
    }
    j["hits"] = hits;
    return j;
}
} // namespace

Result fromJson (const std::string& text)
{
    Result r;
    const auto j = json::parse (text, nullptr, false);
    if (j.is_discarded()) { r.error = "JSON illisible"; return r; }
    const json* list = nullptr;
    json single = json::array();
    if (j.is_object() && j.contains ("format"))
    {
        const auto fmt = j["format"].is_string() ? j["format"].get<std::string>() : std::string();
        if (fmt != "flibuste-motifs" && fmt != "scorsaire-chord-patterns") { r.error = "format inconnu : " + fmt; return r; }
        const auto v = j.value ("version", 0);
        if (v < 1 || v > kFormatVersion) { r.error = "version non prise en charge : " + std::to_string (v); return r; }
        if (! j.contains ("patterns") || ! j["patterns"].is_array()) { r.error = "pas de liste de motifs"; return r; }
        list = &j["patterns"];
    }
    else if (j.is_array())
        list = &j;
    else
    {
        single.push_back (j);
        list = &single;
    }
    for (const auto& pj : *list)
    {
        Pattern p;
        if (readPattern (pj, p)) r.patterns.push_back (std::move (p));
    }
    r.ok = true;
    return r;
}

std::string toJson (const std::vector<Pattern>& patterns, const std::string& from)
{
    json arr = json::array();
    for (const auto& p : patterns) arr.push_back (patternJson (p));
    json j = { { "format", "flibuste-motifs" }, { "version", kFormatVersion }, { "from", from }, { "patterns", arr } };
    return j.dump (1);
}

// ================================================================================================ lecture sur une mesure

std::vector<Hit> hitsForMeasure (const Pattern& p, const Frac& measureLength, int measureIndex)
{
    std::vector<Hit> out;
    if (measureLength.n <= 0) return out;
    const int bars = std::max (1, p.bars);
    const int bar = ((measureIndex % bars) + bars) % bars;
    const Frac barLen = p.barLength();
    const Frac first = makeFrac (barLen.n * bar, barLen.d), last = first + barLen;
    if (barLen.n <= 0) return out;
    std::vector<Hit> source;
    for (const auto& h : p.hits)
        if (compare (h.at, first) >= 0 && compare (h.at, last) < 0)
        {
            Hit c = h;
            c.at = h.at - first;
            source.push_back (c);
        }
    for (Frac start = makeFrac (0, 1); compare (start, measureLength) < 0; start = start + barLen)
        for (const auto& h : source)
        {
            Hit c = h;
            c.at = start + h.at;
            if (compare (c.at, measureLength) >= 0) continue;
            const Frac end = c.at + h.dur;
            if (compare (end, measureLength) > 0) c.dur = measureLength - c.at;
            out.push_back (c);
        }
    return out;
}

// ================================================================================================ degrés

namespace
{
// Degré (1..7) auquel appartient une note de l'accord à `semi` demi-tons de la fondamentale ; `rel` sert à départager 6, 8 et 9.
int degreeOfInterval (int semi, std::uint16_t rel)
{
    auto in = [&] (int s) { return ((rel >> s) & 1) != 0; };
    switch (semi)
    {
        case 0: return 1;
        case 1: case 2: return 2;
        case 3: case 4: return 3;
        case 5: return 4;
        case 6: return in (7) ? 4 : 5;                    // #4 si la quinte juste est là, sinon b5
        case 7: return 5;
        case 8: return in (7) ? 6 : 5;                    // b6 si la quinte juste est là, sinon #5
        case 9: return (in (3) && in (6)) ? 7 : 6;        // bb7 d'un accord diminué
        case 10: case 11: return 7;
        default: return 0;
    }
}
}

int degreeSemitones (const Part& part, std::uint16_t rel, Absent absent)
{
    const int n = part.degree;
    if (part.signedDegree) return majorInterval (n) + part.accidental;
    const int octaves = (n - 1) / 7;
    const int base = (n - 1) % 7 + 1;
    for (int s = 0; s < 12; ++s)
        if (((rel >> s) & 1) != 0 && degreeOfInterval (s, rel) == base) return s + 12 * octaves;
    // 9, 11, 13 : notes au-delà de l'octave de l'accord (bits 12..15 : 12 + k)
    for (int s = 12; s < 16; ++s)
        if (((rel >> s) & 1) != 0 && degreeOfInterval (s - 12, rel) == base && octaves == 1) return s;
    return absent == Absent::Play ? majorInterval (n) : -1;
}

int resolvePart (const Part& part, const ChordContext& chord, Absent absent, int* out, int cap)
{
    int k = 0;
    auto put = [&] (int note) { if (k < cap && note >= 0 && note <= 127) out[k++] = note; };
    switch (part.kind)
    {
        case Part::Kind::Bass:
        {
            int note = chord.rootMidi - 12;
            if (chord.bassPc >= 0)
            {
                const int rootPc = ((chord.rootMidi % 12) + 12) % 12;
                note = chord.rootMidi - 12 + (((chord.bassPc - rootPc) % 12) + 12) % 12;
                if (note > chord.rootMidi - 1) note -= 12;
            }
            put (note);
            break;
        }
        case Part::Kind::Chord:
            for (int s = 0; s < 12; ++s)
                if (((chord.rel >> s) & 1) != 0) put (chord.rootMidi + s);
            break;
        case Part::Kind::Degree:
        {
            const int semi = degreeSemitones (part, chord.rel, absent);
            if (semi >= 0) put (chord.rootMidi + semi + 12 * part.octave);
            break;
        }
    }
    return k;
}
} // namespace neptune::core::pattern
