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

struct NoteIn { int offset = 0; bool on = false; std::uint8_t note = 0; std::uint8_t velocity = 0; };
struct NoteOut { int offset = 0; bool on = false; std::uint8_t note = 0; std::uint8_t velocity = 0; };

struct Transport
{
    bool playing = false;
    bool hasPosition = false;
    double ppq = 0.0;      // position au début du bloc, en noires
    double bpm = 120.0;
};

class Arpeggiator
{
public:
    static constexpr int kMaxHeld = 32;
    static constexpr int kMaxPool = 128;
    static constexpr int kMaxPending = 256;

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

        // ---- horloge
        bool resync = false;
        if (! clockValid_) resync = true;
        else if (syncHost != wasSync_) resync = true;
        else if (syncHost && std::fabs (tr.ppq - expectedNext_) > 1e-3) resync = true;

        if (syncHost && (! wasSync_ || ! clockValid_)) counter_ = 0;   // départ de la lecture : le motif repart du début
        if (resync || releaseAllPending_)
        {
            for (int i = 0; i < pendingCount_; ++i) emit (0, false, pending_[i].note, 0);
            pendingCount_ = 0;
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

        // ---- fusion : fins programmées, événements d'entrée, pas de la grille (dans cet ordre à instant égal)
        int ie = 0;
        const double blockBeats = numSamples * bps;
        for (;;)
        {
            int nextOff = -1, offAt = numSamples;
            for (int i = 0; i < pendingCount_; ++i)
                if (pending_[i].at < offAt) { offAt = pending_[i].at; nextOff = i; }
            const int inAt = ie < nIn ? std::clamp (in[ie].offset, 0, numSamples - 1) : numSamples;
            const double stepT = stepTime (nextIndex_);
            const double stepRel = stepT - start;
            const long long stepSample = std::llround (stepRel / bps);   // arrondi une seule fois : même résultat quelle que soit la taille des blocs
            const int stepAt = stepSample < numSamples ? (int) std::max<long long> (stepSample, 0) : numSamples;

            if (nextOff >= 0 && offAt <= inAt && offAt <= stepAt)
            {
                emit (offAt, false, pending_[nextOff].note, 0);
                pending_[nextOff] = pending_[--pendingCount_];
            }
            else if (ie < nIn && inAt <= stepAt)
            {
                handleInput (in[ie]);
                ++ie;
            }
            else if (stepAt < numSamples)
            {
                doStep (stepAt, bps);
                ++nextIndex_;
            }
            else
                break;
        }

        for (int i = 0; i < pendingCount_; ++i) pending_[i].at -= numSamples;
        expectedNext_ = start + blockBeats;
        if (! syncHost) clock_ = expectedNext_;
        return outN_;
    }

private:
    struct Held { std::uint8_t note = 0, velocity = 0; };
    struct Pending { int at = 0; std::uint8_t note = 0; };

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

    void emit (int at, bool on, std::uint8_t note, std::uint8_t vel)
    {
        if (outN_ >= outCap_) return;
        out_[outN_++] = { at, on, note, vel };
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

    void doStep (int at, double bps)
    {
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

    NoteOut* out_ = nullptr;
    int outN_ = 0, outCap_ = 0;
};
} // namespace neptune::core::arp
