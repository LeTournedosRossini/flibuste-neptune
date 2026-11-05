#pragma once
// Suiveur d'accords : tient l'accord de Carte (carte partagée) comme des notes MIDI tenues, pendant la lecture de l'hôte. Pour un instrument qui n'arpège pas
// (l'orgue Orgamon) : les changements d'accord tombent à l'échantillon près dans le bloc ; à l'arrêt de la lecture, tout est relâché. Fil audio : sans allocation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "neptune/core/Arpeggiator.h"

namespace neptune::core::arp
{
struct FollowEvent
{
    int offset = 0;
    bool on = false;
    int channel = 0;   // 0..15
    int note = 0;
};

class ChordFollower
{
public:
    void setChordChannel (int ch) { chordChannel_ = std::clamp (ch, 0, 15); }
    void setBassChannel (int ch) { bassChannel_ = ch < 0 ? -1 : std::min (ch, 15); }   // -1 : pas de basse
    void setBaseNote (int midi) { baseNote_ = std::clamp (midi, 12, 96); }             // la fondamentale est placée entre cette note et onze demi-tons au-dessus

    // Relâche tout au prochain `process`.
    void reset() { releaseAll_ = true; }
    bool sounding() const { return soundingCount_ > 0; }

    // Retourne le nombre d'événements écrits (notes relâchées avant les nouvelles à un même instant).
    int process (const Environment& env, bool playing, bool hasPosition, double ppq, double bpm, double sampleRate, int numSamples, FollowEvent* out, int cap)
    {
        int n = 0;
        auto emit = [&] (int at, bool on, int ch, int note) { if (n < cap) out[n++] = { at, on, ch, note }; };
        if (releaseAll_ || ! (playing && hasPosition) || numSamples <= 0)
        {
            for (int i = 0; i < soundingCount_; ++i) emit (0, false, sounding_[(size_t) i].channel, sounding_[(size_t) i].note);
            soundingCount_ = 0;
            haveCurrent_ = false;
            releaseAll_ = false;
            return n;
        }
        const double bps = std::max (1.0, bpm) / 60.0 / (sampleRate > 0.0 ? sampleRate : 44100.0);
        const double end = ppq + numSamples * bps;
        double t = ppq;
        for (int guard = 0; guard < 16; ++guard)
        {
            Harmony hm;
            const bool has = env.harmonyAt (std::max (0.0, t) + 1e-9, hm);
            const int at = t <= ppq ? 0 : std::clamp ((int) std::llround ((t - ppq) / bps), 0, numSamples - 1);
            const Key k = has ? Key { true, hm.rootPc, hm.bassPc, hm.rel } : Key {};
            if (! (k == current_) || haveCurrent_ == false)
            {
                for (int i = 0; i < soundingCount_; ++i) emit (at, false, sounding_[(size_t) i].channel, sounding_[(size_t) i].note);
                soundingCount_ = 0;
                if (has) startChord (hm, at, emit);
                current_ = k;
                haveCurrent_ = true;
            }
            double boundary = 1e300;
            if (has) boundary = hm.startBeats + hm.lengthBeats;
            else
            {
                double ns = 0.0;
                if (env.nextChordStart (std::max (0.0, t), ns)) boundary = ns;
            }
            if (boundary >= end - 1e-9 || boundary <= t + 1e-12) break;
            t = boundary;
        }
        return n;
    }

private:
    struct Key
    {
        bool valid = false;
        int root = 0, bass = -1;
        std::uint16_t rel = 0;
        bool operator== (const Key& o) const { return valid == o.valid && (! valid || (root == o.root && bass == o.bass && rel == o.rel)); }
    };
    struct Note { int channel = 0, note = 0; };

    template <typename Emit>
    void startChord (const Harmony& hm, int at, Emit& emit)
    {
        const int root = baseNote_ + (((hm.rootPc - baseNote_) % 12) + 12) % 12;
        for (int s = 0; s < 12; ++s)
            if (((hm.rel >> s) & 1) != 0 && root + s <= 127 && soundingCount_ < (int) sounding_.size())
            {
                sounding_[(size_t) soundingCount_++] = { chordChannel_, root + s };
                emit (at, true, chordChannel_, root + s);
            }
        if (bassChannel_ >= 0 && soundingCount_ < (int) sounding_.size())
        {
            const int pc = hm.bassPc >= 0 ? hm.bassPc : hm.rootPc;
            const int low = baseNote_ - 12;
            const int note = low + (((pc - low) % 12) + 12) % 12;
            if (note >= 0 && note <= 127)
            {
                sounding_[(size_t) soundingCount_++] = { bassChannel_, note };
                emit (at, true, bassChannel_, note);
            }
        }
    }

    int chordChannel_ = 0, bassChannel_ = -1, baseNote_ = 48;
    bool releaseAll_ = false, haveCurrent_ = false;
    Key current_;
    std::array<Note, 24> sounding_ {};
    int soundingCount_ = 0;
};
} // namespace neptune::core::arp
