#include "neptune/core/ChordTimeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace neptune::core::chords
{
namespace
{
struct Quality
{
    const char* suffix;     // identifiant du dictionnaire de Scorsaire : celui qu'on écrit (« » = majeur)
    std::uint32_t rel;      // intervalles sur deux octaves
};

// Dictionnaire de qualités de Scorsaire (tools/gen_qualities.py).
const Quality kQualities[] = {
#include "QualityCatalog.inc"
};

struct Alias
{
    const char* text;
    const char* canonical;
};

// Les alias de Scorsaire et ceux que Carte connaissait déjà (tools/gen_qualities.py).
const Alias kAliases[] = {
#include "QualityAliases.inc"
};

bool isCollisionKey (const std::string& lower)
{
    return lower == "m" || lower == "m7" || lower == "m6" || lower == "m9" || lower == "madd9" || lower == "m7b5" || lower == "mmaj7" || lower == "mm7";
}

std::string lowered (const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return r;
}

// « [0,4,7,13] » : des intervalles en demi-tons (de 0 à 23) ; la fondamentale est toujours là.
bool customQuality (const std::string& s, std::uint32_t& rel)
{
    if (s.size() < 3 || s.front() != '[' || s.back() != ']') return false;
    std::uint32_t r = 1u;
    std::size_t i = 1;
    bool any = false;
    while (i + 1 < s.size())
    {
        std::size_t j = i;
        int v = 0;
        while (j + 1 < s.size() && std::isdigit (static_cast<unsigned char> (s[j]))) { v = v * 10 + (s[j] - '0'); if (v > 99) return false; ++j; }
        if (j == i || v >= kIntervalBits) return false;
        r |= 1u << v;
        any = true;
        i = j;
        if (i + 1 < s.size()) { if (s[i] != ',') return false; ++i; }
    }
    if (! any) return false;
    rel = r;
    return true;
}

bool qualityFromSuffix (const std::string& suffix, std::uint32_t& rel)
{
    if (customQuality (suffix, rel)) return true;
    auto findCanonical = [&] (const std::string& s) -> bool {
        for (const auto& q : kQualities)
            if (s == q.suffix) { rel = q.rel; return true; }
        return false;
    };
    if (findCanonical (suffix)) return true;
    for (const auto& a : kAliases)
        if (suffix == a.text) return findCanonical (a.canonical);
    // 2e passe : sans tenir compte des majuscules, sauf pour « m » / « M » qui n'ont pas le même sens.
    const auto low = lowered (suffix);
    if (isCollisionKey (low)) return false;
    for (const auto& q : kQualities)
        if (low == lowered (q.suffix)) { rel = q.rel; return true; }
    for (const auto& a : kAliases)
        if (low == lowered (a.text)) return findCanonical (a.canonical);
    return false;
}

const char* const kLetters[2][12] = {
    { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" },
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } };
const char* const kFrench[2][12] = {
    { "Do", "R\xC3\xA9" "b", "R\xC3\xA9", "Mib", "Mi", "Fa", "Solb", "Sol", "Lab", "La", "Sib", "Si" },
    { "Do", "Do#", "R\xC3\xA9", "R\xC3\xA9#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" } };

std::string noteName (int pc, bool sharp, Naming n)
{
    pc = ((pc % 12) + 12) % 12;
    return n == Naming::Letters ? kLetters[sharp ? 1 : 0][pc] : kFrench[sharp ? 1 : 0][pc];
}

bool startsWith (const std::string& s, std::size_t pos, const char* prefix)
{
    const std::string p (prefix);
    return s.compare (pos, p.size(), p) == 0;
}

// Lit une note (lettre ou nom français + altération) à partir de `pos`. Avance `pos`.
bool readNote (const std::string& s, std::size_t& pos, int& pc, bool& sharp)
{
    if (pos >= s.size()) return false;
    struct Name { const char* text; int pc; };
    static const Name french[] = { { "sol", 7 }, { "do", 0 }, { "re", 2 }, { "r\xC3\xA9", 2 }, { "mi", 4 }, { "fa", 5 }, { "la", 9 }, { "si", 11 } };
    static const Name letters[] = { { "c", 0 }, { "d", 2 }, { "e", 4 }, { "f", 5 }, { "g", 7 }, { "a", 9 }, { "b", 11 } };
    bool found = false;
    std::size_t p = pos;
    const auto rest = lowered (s.substr (pos));
    for (const auto& n : french)
        if (rest.compare (0, std::string (n.text).size(), n.text) == 0) { pc = n.pc; p = pos + std::string (n.text).size(); found = true; break; }
    if (! found)
        for (const auto& n : letters)
            if (rest[0] == n.text[0]) { pc = n.pc; p = pos + 1; found = true; break; }
    if (! found) return false;
    sharp = false;
    if (p < s.size())
    {
        if (s[p] == '#') { pc += 1; sharp = true; ++p; }
        else if (startsWith (s, p, "\xE2\x99\xAF")) { pc += 1; sharp = true; p += 3; }
        else if (s[p] == 'b') { pc -= 1; ++p; }
        else if (startsWith (s, p, "\xE2\x99\xAD")) { pc -= 1; p += 3; }
    }
    pc = ((pc % 12) + 12) % 12;
    pos = p;
    return true;
}
} // namespace

const std::vector<std::string>& qualitySuffixes()
{
    static const std::vector<std::string> v = [] {
        std::vector<std::string> r;
        for (const auto& q : kQualities) r.push_back (q.suffix);
        return r;
    }();
    return v;
}

bool parseChord (const std::string& textIn, Chord& out)
{
    std::string text;
    for (const char c : textIn)
        if (c != ' ' && c != '\t') text.push_back (c);
    if (text.empty()) return false;

    Chord c;
    std::size_t pos = 0;
    int root = 0;
    bool sharp = false;
    if (! readNote (text, pos, root, sharp)) return false;
    c.root = root;
    c.sharp = sharp;

    const auto slash = text.find ('/', pos);
    const std::string suffix = text.substr (pos, slash == std::string::npos ? std::string::npos : slash - pos);
    bool ok = qualityFromSuffix (suffix, c.rel);
    if (ok && slash != std::string::npos)
    {
        std::size_t bp = slash + 1;
        int bass = 0;
        bool bsharp = false;
        ok = readNote (text, bp, bass, bsharp) && bp == text.size();
        if (ok)
        {
            if (bass != c.root) c.bass = bass;
            if (bsharp) c.sharp = true;
        }
    }
    // « 6/9 » contient une barre : toute la fin du texte est peut-être la qualité (pas de basse)
    if (! ok)
    {
        if (slash == std::string::npos || ! qualityFromSuffix (text.substr (pos), c.rel)) return false;
        c.bass = -1;
        c.sharp = sharp;
    }
    out = c;
    return true;
}

bool qualityIntervals (const std::string& idOrAlias, std::uint32_t& rel) { return qualityFromSuffix (idOrAlias, rel); }

std::string qualityId (std::uint32_t rel)
{
    rel |= 1u;
    for (const auto& q : kQualities)
        if (q.rel == rel) return q.suffix;
    std::string t = "[";
    for (int n = 0; n < kIntervalBits; ++n)
        if ((rel >> n) & 1u) t += (t.size() > 1 ? "," : "") + std::to_string (n);
    return t + "]";
}

std::string formatChord (const Chord& c, Naming naming)
{
    const std::string suffix = qualityId (c.rel);
    std::string s = noteName (c.root, c.sharp, naming) + suffix;
    if (c.bass >= 0 && c.bass != c.root) s += "/" + noteName (c.bass, c.sharp, naming);
    return s;
}

std::uint16_t chordMask (const Chord& c)
{
    std::uint16_t m = 0;
    for (int n = 0; n < kIntervalBits; ++n)
        if ((c.rel & (1u << n)) != 0) m = static_cast<std::uint16_t> (m | (1u << ((c.root + n) % 12)));
    return m;
}

std::uint16_t absoluteMask (const Chord& c)
{
    auto m = chordMask (c);
    if (c.bass >= 0) m = static_cast<std::uint16_t> (m | (1u << (c.bass % 12)));
    return m;
}

namespace
{
constexpr double kEps = 1e-9;

bool validItem (const Item& it) { return std::isfinite (it.start) && std::isfinite (it.length) && it.start >= 0.0 && it.length >= kMinLength; }

int findStart (const std::vector<Item>& items, double start)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        if (std::fabs (items[i].start - start) < kEps) return static_cast<int> (i);
    return -1;
}
} // namespace

int place (std::vector<Item>& items, Item item)
{
    if (! validItem (item)) return -1;
    const double a = item.start, b = item.start + item.length;
    std::vector<Item> out;
    out.reserve (items.size() + 2);
    for (const auto& o : items)
    {
        const double oa = o.start, ob = o.start + o.length;
        if (ob <= a + kEps || oa >= b - kEps) { out.push_back (o); continue; }   // pas de recouvrement
        if (oa < a - kEps)                                                      // le début de l'ancien reste
        {
            Item head = o;
            head.length = a - oa;
            if (head.length >= kMinLength) out.push_back (head);
        }
        if (ob > b + kEps)                                                      // la fin de l'ancien reste
        {
            Item tail = o;
            tail.start = b;
            tail.length = ob - b;
            if (tail.length >= kMinLength) out.push_back (tail);
        }
    }
    out.push_back (item);
    std::sort (out.begin(), out.end(), [] (const Item& x, const Item& y) { return x.start < y.start; });
    items = std::move (out);
    return findStart (items, a);
}

int moveItem (std::vector<Item>& items, int index, double newStart)
{
    if (index < 0 || index >= static_cast<int> (items.size()) || ! std::isfinite (newStart)) return -1;
    Item it = items[static_cast<std::size_t> (index)];
    it.start = std::max (0.0, newStart);
    auto work = items;
    work.erase (work.begin() + index);
    const int i = place (work, it);
    if (i < 0) return -1;
    items = std::move (work);
    return i;
}

int resizeItem (std::vector<Item>& items, int index, double newLength)
{
    if (index < 0 || index >= static_cast<int> (items.size()) || ! std::isfinite (newLength)) return -1;
    Item it = items[static_cast<std::size_t> (index)];
    it.length = std::max (kMinLength, newLength);
    auto work = items;
    work.erase (work.begin() + index);
    const int i = place (work, it);
    if (i < 0) return -1;
    items = std::move (work);
    return i;
}

bool removeItem (std::vector<Item>& items, int index)
{
    if (index < 0 || index >= static_cast<int> (items.size())) return false;
    items.erase (items.begin() + index);
    return true;
}

int indexAt (const std::vector<Item>& items, double beats)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        if (beats >= items[i].start && beats < items[i].start + items[i].length) return static_cast<int> (i);
    return -1;
}

std::string toText (const std::vector<Item>& items)
{
    std::string s;
    for (const auto& it : items)
    {
        char buf[64];
        std::snprintf (buf, sizeof buf, "%.9g %.9g ", it.start, it.length);
        s += buf;
        s += formatChord (it.chord, Naming::Letters);
        s += '\n';
    }
    return s;
}

std::vector<Item> fromText (const std::string& text)
{
    std::vector<Item> items;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        auto end = text.find ('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr (pos, end - pos);
        pos = end + 1;
        char* p = nullptr;
        const double start = std::strtod (line.c_str(), &p);
        if (p == line.c_str()) continue;
        char* q = nullptr;
        const double length = std::strtod (p, &q);
        if (q == p) continue;
        Item it;
        it.start = start;
        it.length = length;
        if (! parseChord (q, it.chord)) continue;
        place (items, it);   // ignore les valeurs invalides et règle les chevauchements d'un fichier abîmé
    }
    return items;
}

std::vector<chordmap::Container> toContainers (const std::vector<Item>& items)
{
    std::vector<chordmap::Container> v;
    v.reserve (items.size());
    for (const auto& it : items)
    {
        chordmap::Container c;
        c.startBeats = it.start;
        c.lengthBeats = it.length;
        c.kind = chordmap::Kind::Chord;
        c.root = static_cast<std::uint8_t> (it.chord.root);
        c.bass = it.chord.bass >= 0 ? static_cast<std::uint8_t> (it.chord.bass) : static_cast<std::uint8_t> (255);
        c.pitchMask = chordMask (it.chord);
        c.setIntervals (it.chord.rel & kIntervalMask);
        v.push_back (c);
    }
    return v;
}
} // namespace neptune::core::chords
