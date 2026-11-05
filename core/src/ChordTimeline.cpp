#include "neptune/core/ChordTimeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace neptune::core::chords
{
namespace
{
std::uint16_t maskOf (std::initializer_list<int> notes)
{
    std::uint16_t m = 0;
    for (const int n : notes) m = static_cast<std::uint16_t> (m | (1u << (n % 12)));
    return m;
}

struct Quality
{
    const char* suffix;     // premier suffixe d'une famille : celui qu'on écrit
    std::uint16_t rel;
};

// Suffixes reconnus. Les synonymes sont dans `kAliases`.
const Quality kQualities[] = {
    { "", maskOf ({ 0, 4, 7 }) },        { "m", maskOf ({ 0, 3, 7 }) },        { "7", maskOf ({ 0, 4, 7, 10 }) },
    { "maj7", maskOf ({ 0, 4, 7, 11 }) }, { "m7", maskOf ({ 0, 3, 7, 10 }) },  { "mMaj7", maskOf ({ 0, 3, 7, 11 }) },
    { "dim", maskOf ({ 0, 3, 6 }) },     { "dim7", maskOf ({ 0, 3, 6, 9 }) },  { "m7b5", maskOf ({ 0, 3, 6, 10 }) },
    { "aug", maskOf ({ 0, 4, 8 }) },     { "7#5", maskOf ({ 0, 4, 8, 10 }) },  { "sus2", maskOf ({ 0, 2, 7 }) },
    { "sus4", maskOf ({ 0, 5, 7 }) },    { "7sus4", maskOf ({ 0, 5, 7, 10 }) }, { "6", maskOf ({ 0, 4, 7, 9 }) },
    { "m6", maskOf ({ 0, 3, 7, 9 }) },   { "9", maskOf ({ 0, 4, 7, 10, 2 }) }, { "maj9", maskOf ({ 0, 4, 7, 11, 2 }) },
    { "m9", maskOf ({ 0, 3, 7, 10, 2 }) }, { "add9", maskOf ({ 0, 4, 7, 2 }) }, { "madd9", maskOf ({ 0, 3, 7, 2 }) },
    { "5", maskOf ({ 0, 7 }) },          { "7b9", maskOf ({ 0, 4, 7, 10, 1 }) }, { "7#9", maskOf ({ 0, 4, 7, 10, 3 }) },
    // comparées au dictionnaire de Scorsaire (5 novembre 2026) : ces huit-là manquaient à Carte
    { "11", maskOf ({ 0, 4, 7, 10, 2, 5 }) }, { "13", maskOf ({ 0, 4, 7, 10, 2, 5, 9 }) }, { "69", maskOf ({ 0, 4, 7, 9, 2 }) },
    { "7#11", maskOf ({ 0, 4, 7, 10, 2, 6 }) }, { "7b5", maskOf ({ 0, 4, 6, 10 }) }, { "7b13", maskOf ({ 0, 4, 7, 10, 8 }) },
    { "alt", maskOf ({ 0, 4, 6, 8, 10, 1, 3 }) },
};

struct Alias
{
    const char* text;
    const char* canonical;
};

const Alias kAliases[] = {
    { "maj", "" }, { "major", "" }, { "min", "m" }, { "mi", "m" }, { "-", "m" }, { "minor", "m" }, { "mineur", "m" }, { "majeur", "" },
    { "M7", "maj7" }, { "ma7", "maj7" }, { "\xCE\x94", "maj7" }, { "\xCE\x94" "7", "maj7" }, { "\xE2\x96\xB3", "maj7" }, { "\xE2\x96\xB3" "7", "maj7" },
    { "min7", "m7" }, { "-7", "m7" }, { "mM7", "mMaj7" }, { "m(maj7)", "mMaj7" }, { "min(maj7)", "mMaj7" },
    { "o", "dim" }, { "\xC2\xB0", "dim" }, { "o7", "dim7" }, { "\xC2\xB0" "7", "dim7" },
    { "\xC3\xB8", "m7b5" }, { "\xC3\xB8" "7", "m7b5" }, { "min7b5", "m7b5" }, { "m7-5", "m7b5" },
    { "+", "aug" }, { "+7", "7#5" }, { "aug7", "7#5" }, { "sus", "sus4" }, { "min6", "m6" }, { "min9", "m9" }, { "maj9", "maj9" }, { "M9", "maj9" },
    { "2", "add9" }, { "add2", "add9" }, { "(add9)", "add9" }, { "mi7", "m7" },
    { "7sus", "7sus4" },
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

bool qualityFromSuffix (const std::string& suffix, std::uint16_t& rel)
{
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
    if (! qualityFromSuffix (suffix, c.rel)) return false;

    if (slash != std::string::npos)
    {
        std::size_t bp = slash + 1;
        int bass = 0;
        bool bsharp = false;
        if (! readNote (text, bp, bass, bsharp) || bp != text.size()) return false;
        if (bass != c.root) c.bass = bass;
        if (bsharp) c.sharp = true;
    }
    out = c;
    return true;
}

std::string formatChord (const Chord& c, Naming naming)
{
    std::string suffix = "?";
    for (const auto& q : kQualities)
        if (q.rel == c.rel) { suffix = q.suffix; break; }
    std::string s = noteName (c.root, c.sharp, naming) + suffix;
    if (c.bass >= 0 && c.bass != c.root) s += "/" + noteName (c.bass, c.sharp, naming);
    return s;
}

std::uint16_t chordMask (const Chord& c)
{
    std::uint16_t m = 0;
    for (int n = 0; n < 12; ++n)
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
        v.push_back (c);
    }
    return v;
}
} // namespace neptune::core::chords
