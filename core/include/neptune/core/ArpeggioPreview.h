#pragma once
// Aperçu d'Arpège (débogage) : rejoue l'arpégiateur HORS LIGNE sur la carte d'accords, comme pendant la lecture, et rend la liste des notes qui seraient jouées
// (avec leurs débuts et fins en noires), les accords de la carte et les accords d'approche. Aucun son, aucun MIDI : sert à contrôler ce qui sera lu.
// Thread : interface (message). Alloue ; ne pas appeler depuis le fil audio.
#include <vector>

#include "neptune/core/Arpeggiator.h"

namespace neptune::core::arp
{
struct PreviewNote
{
    double start = 0.0, end = 0.0;   // en noires de l'hôte
    int note = 60;                   // MIDI
    int velocity = 100;
    bool bass = false;               // partie « B » du motif (le pédalier, dans Orgamon)
    bool approach = false;           // note jouée sur un accord d'approche
};
struct PreviewChord
{
    double start = 0.0, length = 0.0;
    Harmony harmony;                 // accord de la carte
    bool hasApproach = false;
    Harmony approach;                // accord d'approche qui précède le changement (le premier trouvé dans la durée de cet accord)
    double approachFrom = 0.0;       // où il commence
};
struct PreviewSettings
{
    Params params;
    const pattern::Pattern* pattern = nullptr;   // nullptr : motifs automatiques
    pattern::Absent absent = pattern::Absent::Skip;
    const Environment* env = nullptr;            // la carte ; nullptr : rien à jouer
    int baseNote = 48;
    bool ledVoices = false;
    StrumParams strum;
    ApproachParams approach;
    double bpm = 120.0;
    double beatsPerBar = 4.0;                    // si la carte n'a pas de mesures
};
struct Preview
{
    std::vector<PreviewNote> notes;
    std::vector<PreviewChord> chords;
    double start = 0.0, end = 0.0;   // étendue montrée
    bool truncated = false;          // la carte dépasse `maxBeats` ou `maxNotes` : le reste n'est pas montré
};

// Rejoue de `from` à la fin du dernier accord de la carte (au plus `maxBeats` noires, `maxNotes` notes).
Preview renderPreview (const PreviewSettings& s, double from = 0.0, double maxBeats = 256.0, int maxNotes = 6000);
} // namespace neptune::core::arp
