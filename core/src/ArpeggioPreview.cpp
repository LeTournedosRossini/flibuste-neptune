#include "neptune/core/ArpeggioPreview.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace neptune::core::arp
{
Preview renderPreview (const PreviewSettings& s, double from, double maxBeats, int maxNotes)
{
    Preview pv;
    if (s.env == nullptr) return pv;

    // les accords de la carte, de `from` jusqu'au dernier
    double t = std::max (0.0, from);
    double end = t;
    for (int guard = 0; guard < 20000; ++guard)
    {
        Harmony hm;
        if (! s.env->harmonyAt (t, hm))
        {
            double next = 0.0;
            if (! s.env->nextChordStart (t, next) || next <= t) break;
            t = next;
            continue;
        }
        if (hm.lengthBeats <= 0.0) break;
        if (hm.startBeats >= from + maxBeats) { pv.truncated = true; break; }
        PreviewChord pc;
        pc.start = hm.startBeats;
        pc.length = hm.lengthBeats;
        pc.harmony = hm;
        pv.chords.push_back (pc);
        end = std::max (end, hm.startBeats + hm.lengthBeats);
        t = hm.startBeats + hm.lengthBeats;
    }
    if (pv.chords.empty()) return pv;
    pv.start = pv.chords.front().start;
    if (end - pv.start > maxBeats) { end = pv.start + maxBeats; pv.truncated = true; }
    pv.end = end;

    auto a = std::make_unique<Arpeggiator>();
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 256;
    a->setSampleRate (kSampleRate);
    a->setParams (s.params);
    a->setPattern (s.pattern, s.absent);
    a->setEnvironment (s.env);
    a->setBaseNote (s.baseNote);
    a->setVoiceLeading (s.ledVoices);
    a->setStrum (s.strum);
    a->setApproach (s.approach);
    a->reset();

    const double bps = std::max (1.0, s.bpm) / 60.0 / kSampleRate;   // noires par échantillon
    std::vector<NoteOut> out (512);
    struct Open { int note; bool bass; std::size_t index; };
    std::vector<Open> open;
    double ppq = pv.start;
    const double beatsPerBar = s.beatsPerBar > 0.0 ? s.beatsPerBar : 4.0;
    while (ppq < pv.end && static_cast<int> (pv.notes.size()) < maxNotes)
    {
        Transport tr;
        tr.playing = true;
        tr.hasPosition = true;
        tr.ppq = ppq;
        tr.bpm = s.bpm;
        tr.beatsPerBar = beatsPerBar;
        tr.barStartPpq = std::floor (ppq / beatsPerBar) * beatsPerBar;
        const int n = a->process (nullptr, 0, tr, kBlock, out.data(), static_cast<int> (out.size()));
        for (int i = 0; i < n; ++i)
        {
            const auto& e = out[static_cast<std::size_t> (i)];
            const double at = ppq + e.offset * bps;
            if (e.on)
            {
                PreviewNote pn;
                pn.start = at;
                pn.end = at;
                pn.note = e.note;
                pn.velocity = e.velocity;
                pn.bass = e.bass;
                Harmony ap;
                pn.approach = a->approachAt (at, ap);
                pv.notes.push_back (pn);
                open.push_back ({ e.note, e.bass, pv.notes.size() - 1 });
            }
            else
            {
                for (std::size_t k = 0; k < open.size(); ++k)
                    if (open[k].note == e.note && open[k].bass == e.bass)
                    {
                        pv.notes[open[k].index].end = at;
                        open.erase (open.begin() + static_cast<std::ptrdiff_t> (k));
                        break;
                    }
            }
        }
        ppq += kBlock * bps;
    }
    for (const auto& o : open) pv.notes[o.index].end = std::max (pv.notes[o.index].start, std::min (pv.end, pv.notes[o.index].start + 0.25));   // note encore tenue à la fin
    if (static_cast<int> (pv.notes.size()) >= maxNotes) pv.truncated = true;

    // l'accord d'approche qui précède chaque changement : cherché sur les notes qui en jouent
    for (auto& c : pv.chords)
    {
        for (const auto& pn : pv.notes)
            if (pn.approach && pn.start >= c.start && pn.start < c.start + c.length)
            {
                Harmony ap;
                if (a->approachAt (pn.start, ap)) { c.hasApproach = true; c.approach = ap; c.approachFrom = pn.start; }
                break;
            }
    }
    return pv;
}
} // namespace neptune::core::arp
