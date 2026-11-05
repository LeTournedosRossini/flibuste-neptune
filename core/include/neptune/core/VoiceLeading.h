#pragma once
// Conduite de voix : choisit, pour un accord, la position des notes qui bouge le moins par rapport à la précédente (notes communes gardées, voix du haut calme,
// pas de seconde mineure entre voisines, écartement demandé). Port de `comping.js` de Scorsaire (`voiceLeadScored`, `voicingCost`, `bassNote`, `chordTones`) avec les
// mêmes poids. Fil audio : sans allocation (tableaux de taille fixe).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace neptune::core::voice
{
constexpr int kMaxVoices = 8;
constexpr int kHandLow = 55, kHandHigh = 76;   // tessiture de la main droite
constexpr int kBassLow = 36, kBassHigh = 52;   // tessiture de la basse

// Poids des règles (un point = un demi-ton de déplacement) ; à régler à l'oreille.
struct Costs
{
    double move = 1, common = 2, top = 0.5, secondTop = 40, second = 15, sameWay = 3, parallel = 30, wide = 2, spacing = 5, sixth = 10, centre = 0.15;
};
inline const Costs& costs() { static const Costs c; return c; }

enum class Spacing { Auto, Close, Drop2, Drop3 };
enum class Shape { Close, Drop2, Drop3, Other };

struct Rules
{
    double topSmooth = 0;          // 0 : pas du tout, 1 : usuel, 2 : beaucoup
    bool avoidSeconds = false;     // évite le demi-ton (ou la neuvième mineure) entre voisines
    Spacing spacing = Spacing::Auto;
    bool contrary = false;         // la voix du haut ne va pas dans le sens de la basse
    bool noParallels = false;      // pas de quintes ni d'octaves parallèles basse / voix du haut
    static Rules usual() { Rules r; r.topSmooth = 1; r.avoidSeconds = true; return r; }
};

struct Voicing
{
    std::array<int, kMaxVoices> notes {};   // triées, MIDI
    int count = 0;
    double cost = std::numeric_limits<double>::infinity();
    bool empty() const { return count == 0; }
};

struct Context
{
    const int* prev = nullptr;   // notes précédentes (triées ou non), null = aucune
    int prevCount = 0;
    int bass = -1, prevBass = -1;   // -1 : pas de basse
    double centre = 65.5;
};

namespace detail
{
inline int mod12 (int n) { return ((n % 12) + 12) % 12; }
inline void sortSmall (int* a, int n)
{
    for (int i = 1; i < n; ++i)
    {
        const int v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}
inline int sign (int v) { return (v > 0) - (v < 0); }
} // namespace detail

// Écartement d'une position : serrée (dans l'octave), drop 2 / drop 3 (la 2e / 3e note depuis le haut d'une position serrée descendue d'une octave), autre.
inline Shape voicingShape (const int* notes, int count)
{
    std::array<int, kMaxVoices> n {};
    count = std::min (count, kMaxVoices);
    for (int i = 0; i < count; ++i) n[(size_t) i] = notes[i];
    detail::sortSmall (n.data(), count);
    if (count < 3 || n[(size_t) count - 1] - n[0] < 12) return Shape::Close;
    const int raised = n[0] + 12;
    int hi = raised, lo = raised, above = 0;
    for (int i = 1; i < count; ++i)
    {
        hi = std::max (hi, n[(size_t) i]);
        lo = std::min (lo, n[(size_t) i]);
        if (n[(size_t) i] > raised) ++above;
    }
    if (hi - lo >= 12) return Shape::Other;
    const int k = 1 + above;
    return k == 2 ? Shape::Drop2 : k == 3 ? Shape::Drop3 : Shape::Other;
}

// Ce que coûte une position (plus c'est bas, mieux c'est), connaissant la précédente.
inline double voicingCost (const int* notes, int count, const Rules& rules, const Context& ctx)
{
    const Costs& C = costs();
    std::array<int, kMaxVoices> s {};
    count = std::min (count, kMaxVoices);
    for (int i = 0; i < count; ++i) s[(size_t) i] = notes[i];
    detail::sortSmall (s.data(), count);
    const int top = s[(size_t) count - 1];
    double c = 0;
    std::array<int, kMaxVoices> before {};
    int nb = 0;
    if (ctx.prev != nullptr && ctx.prevCount > 0)
    {
        nb = std::min (ctx.prevCount, kMaxVoices);
        for (int i = 0; i < nb; ++i) before[(size_t) i] = ctx.prev[i];
        detail::sortSmall (before.data(), nb);
        double mv = 0;
        if (nb == count)
            for (int i = 0; i < count; ++i) mv += std::abs (s[(size_t) i] - before[(size_t) i]);
        else
            for (int i = 0; i < count; ++i)
            {
                int m = 1000;
                for (int j = 0; j < nb; ++j) m = std::min (m, std::abs (s[(size_t) i] - before[(size_t) j]));
                mv += m;
            }
        c += C.move * mv;
        int common = 0;
        for (int i = 0; i < count; ++i)
            for (int j = 0; j < nb; ++j)
                if (s[(size_t) i] == before[(size_t) j]) { ++common; break; }
        c -= C.common * common;
        c += C.top * rules.topSmooth * std::abs (top - before[(size_t) nb - 1]);
    }
    double sum = 0;
    for (int i = 0; i < count; ++i) sum += s[(size_t) i];
    c += C.centre * std::abs (sum / count - ctx.centre);
    if (rules.avoidSeconds)
        for (int i = 0; i + 1 < count; ++i)
        {
            const int gap = s[(size_t) i + 1] - s[(size_t) i];
            if (gap == 1 || gap == 13) c += i + 2 == count ? C.secondTop : C.second;
        }
    const Shape shape = voicingShape (s.data(), count);
    const auto wanted = rules.spacing == Spacing::Close ? Shape::Close : rules.spacing == Spacing::Drop2 ? Shape::Drop2 : Shape::Drop3;
    if (rules.spacing != Spacing::Auto) { if (count >= 3 && shape != wanted) c += C.spacing; }
    else if (shape == Shape::Other || s[(size_t) count - 1] - s[0] > 12) c += C.wide;
    if (nb > 0 && ctx.bass >= 0 && ctx.prevBass >= 0)
    {
        const int dBass = detail::sign (ctx.bass - ctx.prevBass);
        const int dTop = detail::sign (top - before[(size_t) nb - 1]);
        if (dBass != 0 && dBass == dTop)
        {
            if (rules.contrary) c += C.sameWay;
            if (rules.noParallels)
            {
                const int was = detail::mod12 (before[(size_t) nb - 1] - ctx.prevBass), now = detail::mod12 (top - ctx.bass);
                if (was == now && (now == 7 || now == 0)) c += C.parallel;
            }
        }
    }
    return c;
}

// Position de la main droite d'un accord qui suit la précédente : une note par classe de hauteur dans [low, high], dans l'étendue de deux octaves (relâchée si rien
// ne tient). Vide quand une classe n'a aucune note dans la tessiture.
inline Voicing voiceLead (const int* prev, int prevCount, const int* pcs, int pcCount, int low = kHandLow, int high = kHandHigh, const Rules& rules = {}, int bass = -1, int prevBass = -1)
{
    Voicing none;
    pcCount = std::min (pcCount, kMaxVoices);
    if (pcCount <= 0) return none;
    struct Opts { std::array<int, 8> v {}; int n = 0; };
    std::array<Opts, kMaxVoices> opts {};
    for (int i = 0; i < pcCount; ++i)
    {
        for (int p = low; p <= high && opts[(size_t) i].n < 8; ++p)
            if (detail::mod12 (p) == detail::mod12 (pcs[i])) opts[(size_t) i].v[(size_t) opts[(size_t) i].n++] = p;
        if (opts[(size_t) i].n == 0) return none;
    }
    Context ctx;
    ctx.prev = prev;
    ctx.prevCount = prevCount;
    ctx.bass = bass;
    ctx.prevBass = prevBass;
    ctx.centre = (low + high) / 2.0;
    const int spans[2] = { rules.spacing == Spacing::Drop3 ? 20 : 16, 40 };
    for (int span : spans)
    {
        Voicing best;
        std::array<int, kMaxVoices> acc {};
        // énumération itérative (pas de récursion : fil audio)
        std::array<int, kMaxVoices> idx {};
        int depth = 0;
        while (depth >= 0)
        {
            if (depth == pcCount)
            {
                std::array<int, kMaxVoices> sorted = acc;
                detail::sortSmall (sorted.data(), pcCount);
                if (sorted[(size_t) pcCount - 1] - sorted[0] <= span)
                {
                    const double c = voicingCost (sorted.data(), pcCount, rules, ctx);
                    bool better = c < best.cost - 1e-9;
                    if (! better && std::abs (c - best.cost) < 1e-9 && best.count > 0)
                    {
                        int a = 0, b = 0;
                        for (int i = 0; i < pcCount; ++i) { a += sorted[(size_t) i]; b += best.notes[(size_t) i]; }
                        better = a < b;
                    }
                    if (better) { best.notes = sorted; best.count = pcCount; best.cost = c; }
                }
                --depth;
                continue;
            }
            if (idx[(size_t) depth] < opts[(size_t) depth].n)
            {
                acc[(size_t) depth] = opts[(size_t) depth].v[(size_t) idx[(size_t) depth]++];
                ++depth;
                if (depth < pcCount) idx[(size_t) depth] = 0;
            }
            else { idx[(size_t) depth] = 0; --depth; }
        }
        if (best.count > 0) return best;
    }
    return none;
}

// Basse (MIDI) d'une classe de hauteur dans la tessiture de la basse, la plus proche de la précédente (ou du milieu de la tessiture).
inline int bassNote (int pc, int prev = -1)
{
    const int target = prev >= 0 ? prev : (kBassLow + kBassHigh + 1) / 2;
    int best = -1;
    for (int p = kBassLow; p <= kBassHigh; ++p)
        if (detail::mod12 (p) == detail::mod12 (pc) && (best < 0 || std::abs (p - target) < std::abs (best - target))) best = p;
    return best;
}

// Classes de hauteur jouées par la main droite : fondamentale et notes de l'accord (masque `rel` : bit s = intervalle s au-dessus de la fondamentale) ;
// au-delà de quatre notes, la quinte tombe. `skip` : classe à laisser de côté (la basse, pour un clavier qui la laisse au bassiste), -1 = aucune.
inline int chordTones (int rootPc, std::uint16_t rel, int* out, int skip = -1)
{
    int n = 0;
    auto add = [&] (int pc) { pc = detail::mod12 (pc); if (pc == skip) return; for (int i = 0; i < n; ++i) if (out[i] == pc) return; if (n < kMaxVoices) out[n++] = pc; };
    add (rootPc);
    for (int s = 0; s < 12; ++s) if (((rel >> s) & 1) != 0) add (rootPc + s);
    if (n > 4)
    {
        const int fifth = detail::mod12 (rootPc + 7);
        for (int i = 0; i < n; ++i)
            if (out[i] == fifth) { for (int j = i; j + 1 < n; ++j) out[j] = out[j + 1]; --n; break; }
    }
    return n;
}
} // namespace neptune::core::voice
