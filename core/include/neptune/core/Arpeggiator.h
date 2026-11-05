#pragma once
// Arpégiateur (cœur, sans JUCE) : transforme les notes tenues en une suite de notes calées sur la grille de l'hôte (voir docs/plan-harmonie-arpegiateur.md,
// phase 2). Appelé une fois par bloc audio : entrées = événements de note avec leur instant dans le bloc + transport ; sorties = événements de note avec
// leur instant dans le bloc. Sans allocation, sans verrou : tout est de taille fixe.
//   - Grille en temps de l'hôte (noires) : les pas tombent toujours aux mêmes positions, quel que soit le tempo ; swing sur les pas impairs.
//   - Lecture arrêtée : l'arpégiateur tourne sur sa propre horloge (on peut jouer sans lancer l'hôte) ; il se recale au départ de la lecture.
//   - Saut de position (boucle, clic dans la règle), arrêt, départ : toutes les notes qui sonnent reçoivent leur fin tout de suite. Une note émise a toujours
//     sa fin programmée (durée de note) : jamais de note bloquée.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "neptune/core/ChordPattern.h"
#include "neptune/core/VoiceLeading.h"

namespace neptune::core::arp
{
enum class Pattern : int { Up = 0, Down, UpDown, UpDownRepeat, Random, AsPlayed, Chord, Count };

struct Params
{
    Pattern pattern = Pattern::Up;
    int octaves = 1;               // 1..4
    double stepBeats = 0.25;       // longueur d'un pas en noires : 0.25 = double-croche
    double gate = 0.5;             // durée de la note, part du pas (0.05..1)
    double swing = 0.0;            // retard des pas impairs, part du pas (0..0.75)
    bool hold = false;             // garde les notes après le relâchement jusqu'à un nouvel accord
    bool fixedVelocity = false;    // sinon vélocité de la note jouée
    int velocity = 100;            // 1..127, si fixe
    int accentEvery = 0;           // 0 : pas d'accent ; sinon un pas sur N est renforcé
    int accentBoost = 20;          // ajout de vélocité sur le pas accentué
    int transpose = 0;             // demi-tons, -48..48
};

// Grattage des parties « accord » (C) des motifs : les notes partent l'une après l'autre au lieu de ensemble (valeurs de Scorsaire, `strum.js`).
struct StrumParams
{
    enum class Mode { Off, Down, Alternate };   // Alternate : coup bas sur les temps, haut sur les contretemps
    Mode mode = Mode::Off;
    double delayMs = 0.0;          // délai entre deux notes ; 0 = automatique d'après le tempo (14 ms à 120, borné à 5-30 ms)
    int upStrings = 4;             // nombre de notes aiguës d'un coup haut
    double upVelocity = 0.9;       // vélocité relative d'un coup haut
    double fall = 0.03;            // chute de vélocité à chaque note suivante
    double mutedSeconds = 0.06;    // durée d'une note de coup étouffé
    double mutedVelocity = 0.6;
};

// Accord d'approche (Scorsaire, `approachSpec`) : un coup du motif qui tombe peu avant un changement d'accord joue, à la place de l'accord en cours, un accord qui mène à
// l'accord suivant. Jamais sur le premier coup d'un accord. Le tirage de `chance` ne dépend que de l'instant du changement : le même à chaque lecture.
struct ApproachParams
{
    enum class Kind { Off, Dominant, Tritone, Chromatic, Parallel };
    Kind kind = Kind::Off;       // Dominant : V7 de l'accord suivant ; Tritone : ♭II7 ; Chromatic : majeur un demi-ton plus bas ; Parallel : l'accord suivant lui-même un demi-ton plus haut (ou plus bas)
    double chance = 1.0;         // part des changements qui reçoivent une approche (0..1)
    double beats = 1.0;          // les coups qui commencent dans ce nombre de noires avant le changement
    double below = 0.0;          // avec Parallel : part des approches qui viennent d'en dessous
};

struct NoteIn { int offset = 0; bool on = false; std::uint8_t note = 0; std::uint8_t velocity = 0; };
struct NoteOut { int offset = 0; bool on = false; std::uint8_t note = 0; std::uint8_t velocity = 0; bool bass = false; };   // bass : note d'une partie « B » du motif (Orgamon : canal du pédalier)

struct Transport
{
    bool playing = false;
    bool hasPosition = false;
    double ppq = 0.0;      // position au début du bloc, en noires
    double bpm = 120.0;
    double barStartPpq = 0.0;    // début de la mesure en cours (noires) ; sert à caler la grille des mesures en mode motif
    double beatsPerBar = 4.0;    // longueur d'une mesure en noires
};

// Ce que Carte dit à l'arpégiateur à chaque instant (en noires de l'hôte). Appelé depuis le fil audio : sans verrou, sans allocation.
struct Harmony
{
    int rootPc = 0;                 // fondamentale (0..11)
    int bassPc = -1;                // basse (0..11) ou -1
    std::uint16_t rel = 0x091;      // notes de l'accord relatives à la fondamentale
    bool hasScale = false;
    std::uint16_t scaleRel = 0;     // gamme en vigueur, relative à la fondamentale de l'accord
    double startBeats = 0.0;        // début et longueur du conteneur d'accord de Carte (noires de l'hôte)
    double lengthBeats = 0.0;
};
class Environment
{
public:
    virtual ~Environment() = default;
    virtual bool hasPatterns() const noexcept { return false; }                                  // la voie des motifs de Carte a des éléments
    virtual const pattern::Pattern* patternAt (double) const noexcept { return nullptr; }        // motif demandé par Carte à cet instant (nullptr : celui du réglage)
    virtual bool harmonyAt (double, Harmony&) const noexcept { return false; }                   // accord (et gamme) de Carte à cet instant
    virtual bool measureAt (double, double&, double&, int&) const noexcept { return false; }     // mesure réelle : début, longueur, numéro (depuis 0)
    virtual bool nextChordStart (double, double&) const noexcept { return false; }               // début du premier accord de Carte après cet instant
    virtual bool nextMeasureStart (double, double&) const noexcept { return false; }             // début de la première mesure de Carte après cet instant
};

class Arpeggiator
{
public:
    static constexpr int kMaxHeld = 32;
    static constexpr int kMaxPool = 128;
    static constexpr int kMaxPending = 256;

    // Mode motif : les pas viennent d'un motif en degrés d'accord (ChordPattern.h) au lieu de la grille automatique. `nullptr` : mode automatique.
    // Le motif doit rester valide pendant les appels à `process` ; le changer coupe la file des coups à venir (les notes qui sonnent gardent leur fin).
    void setPattern (const pattern::Pattern* p, pattern::Absent absent = pattern::Absent::Skip)
    {
        if (p != pattern_) lastHitT_ = -1e300;
        pattern_ = p;
        absent_ = absent;
    }
    const pattern::Pattern* pattern() const { return pattern_; }
    // Environnement de Carte (accord, gamme, motif, mesures) ; nullptr : seulement les notes tenues et les mesures de l'hôte.
    void setEnvironment (const Environment* e) { env_ = e; }
    // Octave de base quand Carte joue seule (aucune note tenue) : la fondamentale de l'accord est placée entre cette note (MIDI) et onze demi-tons au-dessus.
    void setBaseNote (int midi) { baseNote_ = std::clamp (midi, 0, 108); }

    // Conduite de voix pour les motifs en degrés : chaque coup est posé au plus près du précédent (les octaves écrites sont ignorées ; la basse « B » reste telle quelle).
    // Vrai : tous les motifs ; faux : seulement ceux qui le demandent (`voices.rh = "led"`).
    void setVoiceLeading (bool all) { ledAll_ = all; }

    void setStrum (const StrumParams& sp)
    {
        strum_ = sp;
        strum_.delayMs = std::clamp (strum_.delayMs, 0.0, 100.0);
        strum_.upStrings = std::clamp (strum_.upStrings, 1, 16);
    }

    void setApproach (const ApproachParams& ap)
    {
        approach_ = ap;
        approach_.chance = std::clamp (approach_.chance, 0.0, 1.0);
        approach_.beats = std::clamp (approach_.beats, 0.125, 4.0);
        approach_.below = std::clamp (approach_.below, 0.0, 1.0);
    }

    void setSampleRate (double sr) { sampleRate_ = sr > 0.0 ? sr : 44100.0; }

    void setParams (const Params& p)
    {
        params_ = p;
        params_.octaves = std::clamp (params_.octaves, 1, 4);
        params_.stepBeats = std::clamp (params_.stepBeats, 1.0 / 64.0, 8.0);
        params_.gate = std::clamp (params_.gate, 0.05, 1.0);
        params_.swing = std::clamp (params_.swing, 0.0, 0.75);
        params_.velocity = std::clamp (params_.velocity, 1, 127);
        params_.accentEvery = std::clamp (params_.accentEvery, 0, 16);
        params_.transpose = std::clamp (params_.transpose, -48, 48);
        if (! params_.hold)   // fin du maintien : on ne garde que les touches réellement enfoncées
        {
            int w = 0;
            for (int i = 0; i < heldCount_; ++i)
                if (down_[held_[i].note]) held_[w++] = held_[i];
            heldCount_ = w;
        }
    }
    const Params& params() const { return params_; }

    // Remet tout à zéro. Les fins des notes qui sonnent sont émises au prochain `process` (à l'instant 0).
    void reset()
    {
        heldCount_ = 0;
        down_.fill (false);
        releaseAllPending_ = true;
        counter_ = 0;
        ledCount_ = 0;
        delayedCount_ = 0;
    }

    int heldCount() const { return heldCount_; }
    int pendingCount() const { return pendingCount_; }

    // Retourne le nombre d'événements écrits dans `out` (sortie triée par instant ; à instant égal, les fins avant les débuts).
    int process (const NoteIn* in, int nIn, const Transport& tr, int numSamples, NoteOut* out, int outCap)
    {
        outN_ = 0;
        out_ = out;
        outCap_ = outCap;
        if (numSamples <= 0) return 0;

        const double bps = std::max (1.0, tr.bpm) / 60.0 / sampleRate_;   // noires par échantillon
        const bool syncHost = tr.playing && tr.hasPosition;
        carteLive_ = syncHost && env_ != nullptr;   // Carte ne joue seule que pendant la lecture (sa position n'a de sens que là)

        // ---- horloge
        bool resync = false;
        if (! clockValid_) resync = true;
        else if (syncHost != wasSync_) resync = true;
        else if (syncHost && std::fabs (tr.ppq - expectedNext_) > 1e-3) resync = true;

        if (syncHost && (! wasSync_ || ! clockValid_)) counter_ = 0;   // départ de la lecture : le motif repart du début
        if (resync || releaseAllPending_)
        {
            for (int i = 0; i < pendingCount_; ++i) emit (0, false, pending_[i].note, 0, pending_[i].bass);
            pendingCount_ = 0;
            delayedCount_ = 0;
            releaseAllPending_ = false;
        }
        if (resync)
        {
            clock_ = syncHost ? tr.ppq : expectedNext_;
            nextIndex_ = firstStepAtOrAfter (clock_);
            clockValid_ = true;
        }
        wasSync_ = syncHost;
        const double start = syncHost ? tr.ppq : clock_;
        if (resync) { lastHitT_ = -1e300; ledCount_ = 0; }   // saut de position, arrêt, départ : plus de mémoire du voicing

        // ---- fusion : fins programmées, événements d'entrée, pas de la grille (dans cet ordre à instant égal)
        int ie = 0;
        const double blockBeats = numSamples * bps;
        int dueN = 0, di = 0;
        const bool patternMode = pattern_ != nullptr || (env_ != nullptr && env_->hasPatterns());
        if (patternMode) dueN = collectDue (start, blockBeats, bps, numSamples, tr);
        for (;;)
        {
            int nextOff = -1, offAt = numSamples;
            for (int i = 0; i < pendingCount_; ++i)
                if (pending_[i].at < offAt) { offAt = pending_[i].at; nextOff = i; }
            int nextDel = -1, delAt = numSamples;   // débuts retardés (grattage)
            for (int i = 0; i < delayedCount_; ++i)
                if (delayed_[(size_t) i].at < delAt) { delAt = delayed_[(size_t) i].at; nextDel = i; }
            const int inAt = ie < nIn ? std::clamp (in[ie].offset, 0, numSamples - 1) : numSamples;
            int stepAt = numSamples;
            if (patternMode)
            {
                if (di < dueN) stepAt = due_[di].at;
            }
            else
            {
                const double stepT = stepTime (nextIndex_);
                const double stepRel = stepT - start;
                const long long stepSample = std::llround (stepRel / bps);   // arrondi une seule fois : même résultat quelle que soit la taille des blocs
                stepAt = stepSample < numSamples ? (int) std::max<long long> (stepSample, 0) : numSamples;
            }

            if (nextOff >= 0 && offAt <= inAt && offAt <= stepAt && offAt <= delAt)
            {
                emit (offAt, false, pending_[nextOff].note, 0, pending_[nextOff].bass);
                pending_[nextOff] = pending_[--pendingCount_];
            }
            else if (nextDel >= 0 && delAt <= inAt && delAt <= stepAt)
            {
                const Delayed dl = delayed_[(size_t) nextDel];
                delayed_[(size_t) nextDel] = delayed_[(size_t) --delayedCount_];
                startNote (std::max (dl.at, 0), dl.note, dl.vel, dl.len, dl.bass);
            }
            else if (ie < nIn && inAt <= stepAt)
            {
                handleInput (in[ie]);
                ++ie;
            }
            else if (stepAt < numSamples)
            {
                if (patternMode)
                {
                    doPatternHit (due_[di], bps);
                    ++di;
                }
                else
                {
                    doStep (stepAt, bps, start + (double) stepAt * bps);
                    ++nextIndex_;
                }
            }
            else
                break;
        }

        for (int i = 0; i < pendingCount_; ++i) pending_[i].at -= numSamples;
        for (int i = 0; i < delayedCount_; ++i) delayed_[(size_t) i].at -= numSamples;
        if (patternMode && dueN > 0) lastHitT_ = std::max (lastHitT_, due_[dueN - 1].t);
        expectedNext_ = start + blockBeats;
        if (! syncHost) clock_ = expectedNext_;
        return outN_;
    }

private:
    // un coup du motif à jouer dans ce bloc
    struct Due
    {
        double t = 0.0;        // temps de l'hôte (noires)
        int at = 0;            // instant dans le bloc (échantillons)
        int order = 0;         // ordre de création (départage deux coups simultanés)
        const pattern::Hit* hit = nullptr;
        const pattern::Pattern* pat = nullptr;
        double durBeats = 0.0; // longueur (noires), coupée à la barre
    };
    static constexpr int kMaxDue = 2048;

    // Variante tirée pour cette mesure du morceau et cette répétition du motif : le tirage ne dépend que de leurs numéros et du motif, donc il est le même quelle que soit
    // la taille des blocs et à chaque lecture. `chance` : probabilité ; 0 : jamais (la variante « sur signal » n'a pas de signal ici).
    static const pattern::Alt* pickAlt (const pattern::Pattern& p, int measureIndex, int repIndex)
    {
        if (p.alts.empty()) return nullptr;
        std::uint64_t x = (std::uint64_t) (std::uint32_t) measureIndex * 0x9E3779B97F4A7C15ull ^ ((std::uint64_t) repIndex + 1) * 0xBF58476D1CE4E5B9ull;
        for (const char c : p.id) x = (x ^ (unsigned char) c) * 1099511628211ull;
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        const double u = (double) (z >> 11) * (1.0 / 9007199254740992.0);
        double acc = 0.0;
        for (const auto& a : p.alts)
        {
            acc += a.chance;
            if (u < acc) return &a;
        }
        return nullptr;
    }

    // Coups des motifs dont l'instant tombe dans [start, start + blockBeats), triés. Règle de Scorsaire : la mesure k joue la mesure k modulo `bars` du motif,
    // le motif se répète pour remplir la mesure, un coup qui dépasse la barre est coupé. Les mesures sont celles de Carte (réelles : anacrouse, 3/4...) si elle est là,
    // sinon celles de l'hôte ; le motif de chaque mesure est celui que Carte demande à son début, sinon celui du réglage.
    int collectDue (double start, double blockBeats, double bps, int numSamples, const Transport& tr)
    {
        const double bb = std::max (0.25, tr.beatsPerBar);
        const double origin = tr.barStartPpq - std::floor (tr.barStartPpq / bb + 0.5) * bb;   // décalage de la grille des mesures de l'hôte (0 en général)
        const double end = start + blockBeats;
        int n = 0;
        double t = start - bps;   // une demi-mesure d'avance suffit : on cherche la mesure qui contient le début du bloc
        for (int guard = 0; guard < 64; ++guard)
        {
            double mStart = 0.0, mLen = 0.0;
            int mIndex = 0;
            bool skip = false;
            if (env_ == nullptr || ! env_->measureAt (std::max (0.0, t), mStart, mLen, mIndex) || mLen <= 0.0)
            {
                const auto mi = (long long) std::floor ((t - origin) / bb + 1e-9);   // grille de l'hôte (avant et après les mesures de Carte)
                mStart = origin + (double) mi * bb;
                mLen = bb;
                mIndex = (int) mi;
                if (env_ != nullptr)
                {
                    double es = 0.0, el = 0.0;
                    int ei = 0;
                    if (env_->measureAt (std::max (0.0, mStart + 1e-9), es, el, ei)) skip = true;          // cette mesure de l'hôte commence dans une mesure de Carte : déjà jouée
                    double ns = 0.0;
                    if (env_->nextMeasureStart (std::max (0.0, mStart + 1e-9), ns) && ns > mStart) mLen = std::min (mLen, ns - mStart);   // coupée au début des mesures de Carte
                }
            }
            if (mStart >= end) break;
            const pattern::Pattern* pat = env_ != nullptr ? env_->patternAt (mStart + 1e-9) : nullptr;
            if (pat == nullptr) pat = pattern_;
            if (! skip && pat != nullptr && ! pat->hits.empty())
            {
                const double barBeats = pat->barLength().toBeats();
                const int bars = std::max (1, pat->bars);
                const int bar = ((mIndex % bars) + bars) % bars;
                const double first = barBeats * bar, last = first + barBeats;
                const double mEnd = mStart + mLen;
                if (barBeats > 0.0)
                    for (double rep = 0.0; rep < mLen - 1e-9; rep += barBeats)   // le motif se répète pour remplir la mesure
                    {
                        const pattern::Alt* alt = pickAlt (*pat, mIndex, (int) std::llround (rep / barBeats));
                        const double fromBeats = alt != nullptr ? alt->from.toBeats() : 1e300;
                        auto add = [&] (const pattern::Hit& h, double rel)
                        {
                            const double ht = mStart + rep + rel;
                            if (rep + rel >= mLen - 1e-9) return;
                            const long long sample = std::llround ((ht - start) / bps);
                            if (sample < -1 || sample >= numSamples || ht <= lastHitT_ + 1e-9 || n >= kMaxDue) return;
                            due_[n++] = { ht, (int) std::max<long long> (sample, 0), n, &h, pat, std::min (h.dur.toBeats(), mEnd - ht) };
                        };
                        for (const auto& h : pat->hits)
                        {
                            const double hAt = h.at.toBeats();
                            if (hAt < first - 1e-9 || hAt >= last - 1e-9) continue;
                            if (hAt - first >= fromBeats - 1e-9) continue;   // remplacé par la variante
                            add (h, hAt - first);
                        }
                        if (alt != nullptr)
                            for (const auto& h : alt->hits) add (h, h.at.toBeats());
                    }
            }
            const double next = mStart + mLen;
            if (next <= t) break;
            t = next;
        }
        std::sort (due_.begin(), due_.begin() + n, [] (const Due& a, const Due& b) { return a.t < b.t || (a.t == b.t && a.order < b.order); });
        return n;
    }

    // Remplace l'accord `hm` par son accord d'approche quand le coup à `t` tombe juste avant un changement d'accord (voir ApproachParams).
    void applyApproach (double t, Harmony& hm) const
    {
        if (approach_.kind == ApproachParams::Kind::Off || env_ == nullptr || hm.lengthBeats <= 0.0) return;
        const double until = hm.startBeats + hm.lengthBeats;
        if (t <= hm.startBeats + 1e-6 || until - t > approach_.beats + 1e-6) return;
        Harmony nx;
        if (! env_->harmonyAt (until + 1e-9, nx) || nx.rootPc == hm.rootPc) return;
        // tirages (chance, sens du parallèle) fixés par l'instant du changement : le même à chaque lecture, quelle que soit la taille des blocs
        auto unit = [&] (std::uint32_t salt)
        {
            std::uint32_t x = (std::uint32_t) std::llround (until * 1000.0) * 2654435761u ^ salt * 40503u;
            x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
            return (double) (x & 0xFFFFFF) / (double) 0x1000000;
        };
        if (approach_.chance < 1.0 && unit (1) >= approach_.chance) return;
        const auto mod12 = [] (int n) { return ((n % 12) + 12) % 12; };
        Harmony a;
        a.startBeats = hm.startBeats;
        a.lengthBeats = hm.lengthBeats;
        switch (approach_.kind)
        {
            case ApproachParams::Kind::Dominant:  a.rootPc = mod12 (nx.rootPc + 7); a.rel = 0x491; break;
            case ApproachParams::Kind::Tritone:   a.rootPc = mod12 (nx.rootPc + 1); a.rel = 0x491; break;
            case ApproachParams::Kind::Chromatic: a.rootPc = mod12 (nx.rootPc - 1); a.rel = 0x091; break;
            case ApproachParams::Kind::Parallel:  a.rootPc = mod12 (nx.rootPc + (unit (2) < approach_.below ? -1 : 1)); a.rel = nx.rel; break;
            default: return;
        }
        hm = a;
    }

    void doPatternHit (const Due& d, double bps)
    {
        const CarteChordPool synth (*this, d.t);
        if (heldCount_ == 0 || d.hit == nullptr || d.pat == nullptr) return;
        const auto& h = *d.hit;
        int notes[kMaxHeld];
        int vsum = 0;
        for (int i = 0; i < heldCount_; ++i) { notes[i] = held_[i].note; vsum += held_[i].velocity; }
        std::array<int, kMaxHeld> sorted {};
        for (int i = 0; i < heldCount_; ++i) sorted[(size_t) i] = notes[i];
        std::sort (sorted.begin(), sorted.begin() + heldCount_);
        // l'accord : celui de Carte s'il y en a un, sinon reconnu dans les notes tenues
        pattern::ChordContext ctx;
        Harmony hm;
        if (! d.pat->notesMode && env_ != nullptr && env_->harmonyAt (d.t, hm))
        {
            applyApproach (d.t, hm);
            const int low = sorted[0];
            int root = low - (((low - hm.rootPc) % 12) + 12) % 12;       // la fondamentale la plus proche en dessous (ou égale) de la note la plus grave
            if (low - root > 6) root += 12;                              // ... ou au-dessus si c'est plus près
            ctx.rootMidi = root;
            ctx.rel = hm.rel;
            ctx.bassPc = hm.bassPc;
            ctx.scaleRel = hm.hasScale ? hm.scaleRel : 0;
        }
        else
            ctx = pattern::contextFromNotes (notes, heldCount_);
        const int heldVel = std::clamp (vsum / heldCount_, 1, 127);
        int vel = h.vel > 0 ? h.vel : (params_.fixedVelocity ? params_.velocity : heldVel);
        if (h.accent) vel += params_.accentBoost;
        vel = std::clamp (vel, 1, 127);
        const double gate = h.gate > 0.0 ? h.gate : params_.gate;
        const int len = std::max (1, (int) std::llround (gate * d.durBeats / bps));
        const bool led = ! d.pat->notesMode && (ledAll_ || d.pat->voicesLed);
        std::array<int, voice::kMaxVoices> ledNotes {};
        int ledN = 0;
        if (led)
        {
            // classes de hauteur de toutes les parts de la main droite (accord, degrés), puis une position conduite depuis le coup précédent
            int pcs[voice::kMaxVoices], np = 0;
            for (const auto& part : h.parts)
            {
                if (part.kind == pattern::Part::Kind::Bass || part.leftHand) continue;
                int out[16];
                const int n = pattern::resolvePart (part, ctx, absent_, out, 16);
                for (int k = 0; k < n; ++k)
                {
                    const int pc = ((out[k] % 12) + 12) % 12;
                    bool seen = false;
                    for (int i = 0; i < np; ++i) seen = seen || pcs[i] == pc;
                    if (! seen && np < voice::kMaxVoices) pcs[np++] = pc;
                }
            }
            const int anchor = (synth.active ? baseNote_ : (sorted[0] / 12) * 12);   // tessiture 55..76 pour une base de 48
            const auto v = voice::voiceLead (ledCount_ > 0 ? ledPrev_.data() : nullptr, ledCount_, pcs, np, voice::kHandLow + anchor - 48, voice::kHandHigh + anchor - 48, voice::Rules::usual());
            if (v.count > 0)
            {
                ledNotes = v.notes;
                ledN = v.count;
                ledPrev_ = v.notes;
                ledCount_ = v.count;
            }
        }
        int played[32], nPlayed = 0;
        int chordNotes[16], nChord = 0;   // notes des parties « accord » à gratter
        const bool strumming = strum_.mode != StrumParams::Mode::Off;
        for (const auto& part : h.parts)
        {
            int out[16];
            const int n = d.pat->notesMode ? pattern::resolveRank (part, sorted.data(), heldCount_, out, 16) : pattern::resolvePart (part, ctx, absent_, out, 16);
            const bool useLed = ledN > 0 && part.kind != pattern::Part::Kind::Bass && ! part.leftHand;
            for (int k = 0; k < n; ++k)
            {
                int note = out[k];
                if (strumming && part.kind == pattern::Part::Kind::Chord)
                {
                    if (useLed)
                    {
                        const int pc = ((note % 12) + 12) % 12;
                        for (int i = 0; i < ledN; ++i) if (ledNotes[(size_t) i] % 12 == pc) { note = ledNotes[(size_t) i]; break; }
                    }
                    bool dup = false;
                    for (int i = 0; i < nChord; ++i) dup = dup || chordNotes[i] == note;
                    if (! dup && nChord < 16) chordNotes[nChord++] = note;
                    continue;
                }
                if (useLed)
                {
                    const int pc = ((note % 12) + 12) % 12;
                    for (int i = 0; i < ledN; ++i) if (ledNotes[(size_t) i] % 12 == pc) { note = ledNotes[(size_t) i]; break; }
                    bool dup = false;
                    for (int i = 0; i < nPlayed; ++i) dup = dup || played[i] == note;
                    if (dup) continue;
                    if (nPlayed < 32) played[nPlayed++] = note;
                }
                startNote (d.at, std::clamp (note + params_.transpose, 0, 127), vel, len, part.kind == pattern::Part::Kind::Bass);
            }
        }
        if (nChord > 0) strumChord (d, h, chordNotes, nChord, vel, len, bps);
    }

    // Une partie « accord » grattée : coup bas (de la plus grave à la plus aiguë) ou haut (les notes aiguës, de l'aiguë à la grave), vélocité qui décroît, fin commune.
    void strumChord (const Due& d, const pattern::Hit& h, int* notes, int n, int vel, int len, double bps)
    {
        std::sort (notes, notes + n);
        bool up = false;
        if (strum_.mode == StrumParams::Mode::Alternate)
        {
            const double pos = h.at.toBeats();
            up = std::fabs (pos - std::round (pos)) > 1e-6;   // contretemps
        }
        int order[16], m = n;
        if (up)
        {
            m = std::min (n, strum_.upStrings);
            for (int i = 0; i < m; ++i) order[i] = notes[n - 1 - i];
        }
        else
            for (int i = 0; i < n; ++i) order[i] = notes[i];
        const double bpm = bps * 60.0 * sampleRate_;
        const double delayMs = strum_.delayMs > 0.0 ? strum_.delayMs : std::clamp (std::round (14.0 * 120.0 / std::max (1.0, bpm)), 5.0, 30.0);
        const int delay = std::max (1, (int) std::llround (delayMs * 0.001 * sampleRate_));
        const bool muted = hitMuted (h);
        const double base = (up ? strum_.upVelocity : 1.0) * (muted ? strum_.mutedVelocity : 1.0);
        for (int i = 0; i < m; ++i)
        {
            const int off = std::min (i * delay, len / 2);
            int l = len - off;
            if (muted) l = std::min (l, std::max (1, (int) std::llround (strum_.mutedSeconds * sampleRate_)));
            const int v = std::clamp ((int) std::llround (vel * std::max (0.05, base * (1.0 - strum_.fall * i))), 1, 127);
            const int note = std::clamp (order[i] + params_.transpose, 0, 127);
            if (off == 0) startNote (d.at, note, v, l);
            else if (delayedCount_ < kMaxDelayed) delayed_[(size_t) delayedCount_++] = { d.at + off, (std::uint8_t) note, (std::uint8_t) v, l };
        }
    }
    static bool hitMuted (const pattern::Hit& h) { return h.muted; }

    // Début de note avec sa fin programmée ; une note identique encore en cours est terminée d'abord.
    void startNote (int at, int note, int vel, int len, bool bass = false)
    {
        for (int i = 0; i < pendingCount_; ++i)
            if (pending_[i].note == note && pending_[i].bass == bass)
            {
                emit (at, false, (std::uint8_t) note, 0, bass);
                pending_[i] = pending_[--pendingCount_];
                break;
            }
        if (outN_ >= outCap_ || pendingCount_ >= kMaxPending) return;   // pas de début sans fin programmée
        emit (at, true, (std::uint8_t) note, (std::uint8_t) vel, bass);
        pending_[pendingCount_++] = { at + len, (std::uint8_t) note, bass };
    }

    struct Delayed { int at = 0; std::uint8_t note = 0, vel = 0; int len = 0; bool bass = false; };
    static constexpr int kMaxDelayed = 128;
    struct Held { std::uint8_t note = 0, velocity = 0; };
    struct Pending { int at = 0; std::uint8_t note = 0; bool bass = false; };

    double stepTime (std::int64_t n) const
    {
        return (double) n * params_.stepBeats + ((n & 1) != 0 ? params_.swing * params_.stepBeats : 0.0);
    }
    std::int64_t firstStepAtOrAfter (double beats) const
    {
        auto n = (std::int64_t) std::floor (beats / params_.stepBeats) - 1;
        while (stepTime (n) < beats - 1e-9) ++n;
        return n;
    }

    void emit (int at, bool on, std::uint8_t note, std::uint8_t vel, bool bass = false)
    {
        if (outN_ >= outCap_) return;
        out_[outN_++] = { at, on, note, vel, bass };
    }

    void handleInput (const NoteIn& e)
    {
        if (e.on && e.velocity > 0)
        {
            if (params_.hold && physicalDown() == 0 && heldCount_ > 0) heldCount_ = 0;   // nouvel accord : remplace le précédent
            const bool wasEmpty = heldCount_ == 0;
            bool found = false;
            for (int i = 0; i < heldCount_; ++i)
                if (held_[i].note == e.note) { held_[i].velocity = e.velocity; found = true; }
            if (! found && heldCount_ < kMaxHeld) held_[heldCount_++] = { e.note, e.velocity };
            down_[e.note & 127] = true;
            if (wasEmpty) counter_ = 0;
        }
        else
        {
            down_[e.note & 127] = false;
            if (! params_.hold)
            {
                int w = 0;
                for (int i = 0; i < heldCount_; ++i)
                    if (held_[i].note != e.note) held_[w++] = held_[i];
                heldCount_ = w;
            }
        }
    }
    int physicalDown() const
    {
        int n = 0;
        for (bool d : down_) n += d ? 1 : 0;
        return n;
    }

    // Notes du motif : les notes tenues sur `octaves` octaves. Triées par hauteur (ordre de saisie pour AsPlayed).
    int buildPool (std::array<Held, kMaxPool>& pool) const
    {
        std::array<Held, kMaxHeld> base {};
        for (int i = 0; i < heldCount_; ++i) base[i] = held_[i];
        if (params_.pattern != Pattern::AsPlayed)
            std::sort (base.begin(), base.begin() + heldCount_, [] (const Held& a, const Held& b) { return a.note < b.note; });
        int n = 0;
        for (int o = 0; o < params_.octaves; ++o)
            for (int i = 0; i < heldCount_ && n < kMaxPool; ++i)
            {
                const int note = base[i].note + 12 * o;
                if (note <= 127) pool[n++] = { (std::uint8_t) note, base[i].velocity };
            }
        return n;
    }

    // Sans note tenue, pendant la lecture, l'accord de Carte tient lieu de notes tenues (ses notes à partir de l'octave de base) pour la durée d'un pas.
    struct CarteChordPool
    {
        CarteChordPool (Arpeggiator& a, double t) : self (a)
        {
            Harmony hm;
            if (a.heldCount_ != 0 || ! a.carteLive_ || a.env_ == nullptr || ! a.env_->harmonyAt (t, hm)) return;
            const int low = a.baseNote_;
            const int root = low + (((hm.rootPc - low) % 12) + 12) % 12;
            const auto vel = static_cast<std::uint8_t> (a.params_.fixedVelocity ? a.params_.velocity : 90);
            for (int s = 0; s < 12 && a.heldCount_ < kMaxHeld; ++s)
                if (((hm.rel >> s) & 1) != 0 && root + s <= 127) a.held_[(size_t) a.heldCount_++] = { static_cast<std::uint8_t> (root + s), vel };
            active = a.heldCount_ > 0;
        }
        ~CarteChordPool() { if (active) self.heldCount_ = 0; }
        Arpeggiator& self;
        bool active = false;
    };

    void doStep (int at, double bps, double t)
    {
        const CarteChordPool pool_ (*this, t);
        if (heldCount_ == 0) return;
        std::array<Held, kMaxPool> pool {};
        const int n = buildPool (pool);
        if (n == 0) return;

        const std::int64_t c = counter_++;
        const bool accent = params_.accentEvery > 0 && (c % params_.accentEvery) == 0;
        auto play = [&] (const Held& h)
        {
            const int note = std::clamp ((int) h.note + params_.transpose, 0, 127);
            int vel = params_.fixedVelocity ? params_.velocity : h.velocity;
            if (accent) vel += params_.accentBoost;
            vel = std::clamp (vel, 1, 127);
            for (int i = 0; i < pendingCount_; ++i)   // même note encore en cours : on la termine d'abord
                if (pending_[i].note == note)
                {
                    emit (at, false, (std::uint8_t) note, 0);
                    pending_[i] = pending_[--pendingCount_];
                    break;
                }
            if (outN_ >= outCap_ || pendingCount_ >= kMaxPending) return;   // pas de début sans fin programmée
            emit (at, true, (std::uint8_t) note, (std::uint8_t) vel);
            const int len = std::max (1, (int) std::llround (params_.gate * params_.stepBeats / bps));
            pending_[pendingCount_++] = { at + len, (std::uint8_t) note };
        };

        switch (params_.pattern)
        {
            case Pattern::Chord:
                for (int i = 0; i < n; ++i) play (pool[i]);
                break;
            case Pattern::Down: play (pool[n - 1 - (int) (c % n)]); break;
            case Pattern::Random: play (pool[nextRandom() % (std::uint32_t) n]); break;
            case Pattern::UpDown:
            {
                const int period = n <= 2 ? n : 2 * n - 2;
                const int k = (int) (c % period);
                play (pool[k < n ? k : period - k]);
                break;
            }
            case Pattern::UpDownRepeat:
            {
                const int k = (int) (c % (2 * n));
                play (pool[k < n ? k : 2 * n - 1 - k]);
                break;
            }
            case Pattern::Up:
            case Pattern::AsPlayed:
            default: play (pool[(int) (c % n)]); break;
        }
    }

    std::uint32_t nextRandom()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    Params params_;
    double sampleRate_ = 44100.0;
    std::array<Held, kMaxHeld> held_ {};
    int heldCount_ = 0;
    std::array<bool, 128> down_ {};
    std::array<Pending, kMaxPending> pending_ {};
    int pendingCount_ = 0;
    bool releaseAllPending_ = false;
    std::int64_t counter_ = 0, nextIndex_ = 0;
    double clock_ = 0.0, expectedNext_ = 0.0;
    bool clockValid_ = false, wasSync_ = false;
    std::uint32_t rng_ = 2463534242u;
    const pattern::Pattern* pattern_ = nullptr;
    const Environment* env_ = nullptr;
    bool carteLive_ = false;
    int baseNote_ = 48;
    StrumParams strum_;
    ApproachParams approach_;
    std::array<Delayed, kMaxDelayed> delayed_ {};
    int delayedCount_ = 0;
    bool ledAll_ = false;
    std::array<int, voice::kMaxVoices> ledPrev_ {};
    int ledCount_ = 0;
    pattern::Absent absent_ = pattern::Absent::Skip;
    double lastHitT_ = -1e300;
    std::array<Due, kMaxDue> due_ {};

    NoteOut* out_ = nullptr;
    int outN_ = 0, outCap_ = 0;
};
} // namespace neptune::core::arp
