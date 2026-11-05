#pragma once
// Pont entre la carte partagée (SharedChordMap.h) et l'arpégiateur (Arpeggiator.h) : ce que Carte publie (accord, gamme, tonalité, motif, mesures) devient l'« environnement »
// que l'arpégiateur consulte à chaque instant. Fil audio : sans verrou, sans allocation.
#include <array>
#include <cstdint>
#include <memory>

#include "neptune/core/Arpeggiator.h"
#include "neptune/core/ChordPattern.h"
#include "neptune/core/SharedChordMap.h"

namespace neptune::core::arp
{
constexpr int kPatternSlots = 16;

class ChordMapEnvironment : public Environment
{
public:
    using Slots = std::array<std::shared_ptr<const pattern::Pattern>, kPatternSlots>;
    // `slots` : copie des emplacements de motifs propre au fil audio ; doit rester valide tant que cet objet sert.
    ChordMapEnvironment (const chordmap::Reader& reader, const Slots& slots) : reader_ (reader), slots_ (slots) {}

    bool hasPatterns() const noexcept override { return reader_.count (chordmap::Kind::Pattern) > 0; }

    const pattern::Pattern* patternAt (double t) const noexcept override
    {
        chordmap::Container c;
        if (! reader_.lookup (t, chordmap::Kind::Pattern, c) || c.root < 1 || c.root > kPatternSlots) return nullptr;
        return slots_[static_cast<std::size_t> (c.root) - 1].get();
    }

    bool harmonyAt (double t, Harmony& out) const noexcept override
    {
        chordmap::Container c;
        if (! reader_.lookup (t, chordmap::Kind::Chord, c)) return false;
        out.rootPc = c.root;
        out.bassPc = c.bass <= 11 ? c.bass : -1;
        out.rel = relativeMask (c.pitchMask, c.root);
        if (out.rel == 0) out.rel = 1;
        out.hasScale = false;
        out.scaleRel = 0;
        chordmap::Container sc;
        if (reader_.lookup (t, chordmap::Kind::Scale, sc) || reader_.lookup (t, chordmap::Kind::Key, sc))   // la gamme de Carte, sinon la tonalité
        {
            out.scaleRel = relativeMask (sc.pitchMask, c.root);
            out.hasScale = out.scaleRel != 0;
        }
        return true;
    }

    bool measureAt (double t, double& start, double& length, int& index) const noexcept override
    {
        chordmap::Container c;
        const int i = reader_.indexAt (t, chordmap::Kind::Measure);
        if (i < 0 || ! reader_.lookup (t, chordmap::Kind::Measure, c)) return false;
        start = c.startBeats;
        length = c.lengthBeats;
        index = i;
        return true;
    }

    bool nextMeasureStart (double t, double& start) const noexcept override
    {
        chordmap::Container c;
        if (! reader_.next (t, chordmap::Kind::Measure, c)) return false;
        start = c.startBeats;
        return true;
    }

private:
    // masque absolu (do = 0) -> masque relatif à `root`
    static std::uint16_t relativeMask (std::uint16_t absolute, int root) noexcept
    {
        std::uint16_t rel = 0;
        for (int s = 0; s < 12; ++s)
            if ((absolute >> ((root + s) % 12)) & 1u) rel = static_cast<std::uint16_t> (rel | (1u << s));
        return rel;
    }

    const chordmap::Reader& reader_;
    const Slots& slots_;
};
} // namespace neptune::core::arp
