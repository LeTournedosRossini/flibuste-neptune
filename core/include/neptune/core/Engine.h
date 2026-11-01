#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "neptune/core/DiskStreamer.h"
#include "neptune/core/HandoffSlot.h"
#include "neptune/core/NoteRouter.h"
#include "neptune/core/Instrument.h"
#include "neptune/core/InstrumentSlot.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/Profile.h"
#include "neptune/core/SamplerVoices.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/FmVoices.h"
#include "neptune/core/OrganEngine.h"
#include "neptune/core/Loudness.h"
#include "neptune/core/MidiFilter.h"
#include "neptune/core/PreviewSample.h"
#include "neptune/core/Synth.h"
#include "neptune/core/SynthVoices.h"

namespace neptune::core
{
// Frontière unique entre le cœur et les adaptateurs (JUCE/CLAP, tests, futur web).
// Palier 1 : N pistes, Sampler ou Synthé (chacune son instrument ou son patch, ses voix, son canal MIDI), 16 bus, un Master ; sorties directes sur
// jusqu'à 32 paires stéréo. Les voix d'une piste ne sont jamais celles d'une autre ; le thread disque et ses emplacements de
// streaming sont, eux, partagés (pool).
enum class StripKind { Track, Bus, Master, Parallel };

class Engine
{
public:
    static constexpr int kTracks = mixer::numTracks;
    static constexpr int kBuses = mixer::numBuses;
    static constexpr int kInserts = mixer::numInserts;
    static constexpr int kParallels = mixer::numParallels;

    Engine() noexcept;

    void prepare (double sampleRate, int maxBlockSize) noexcept;

    // Notes de l'hôte : réparties entre les pistes selon leur canal MIDI (0 = toutes). `channel` : 0..15.
    void noteOn (int channel, int key, float velocity) noexcept;
    void noteOff (int channel, int key) noexcept;
    // Routeur de notes (accouplements, divisions de clavier) : voir NoteRouter.h. Thread de message. Résolu quand les paramètres sont rattachés.
    void setRouter (const router::Config&);
    // Notes adressées à UNE piste (clavier de l'interface), sans filtre de canal.
    void noteOnTrack (int track, int channel, int key, float velocity) noexcept;
    void noteOffTrack (int track, int channel, int key) noexcept;
    // Notes d'un clavier de l'interface (Façade, clavier de l'orgue) : jouées sur la piste `track` comme noteOnTrack, ET passées par le routeur comme si elles
    // arrivaient du MIDI sur `channel` (les accouplements donnent la note à d'autres pistes).
    void noteOnKeyboard (int track, int channel, int key, float velocity) noexcept;
    void noteOffKeyboard (int track, int channel, int key) noexcept;
    void panic() noexcept;      // MIDI All Sound Off / bouton « tout arrêter » : toutes les pistes
    void releaseAll() noexcept; // MIDI All Notes Off
    void setParameter (std::uint32_t id, double value) noexcept;
    void process (float* const* out, int numChannels, int numFrames) noexcept;
    // Entrée audio de l'hôte (stéréo) pour le prochain appel à process() : pointeurs valides pendant cet appel seulement, `frames` échantillons ;
    // nuls : pas d'entrée. Alimente les pistes de type Entrée audio dont le réglage « entrée de l'hôte » est actif.
    void setHostInput (const float* left, const float* right, int frames) noexcept { hostL_ = left; hostR_ = right; hostFrames_ = frames; }
    // Profil Live/Studio effectif de l'instance ; appelé par l'adaptateur avant chaque bloc (sans coût). Aucun effet audible pour l'instant.
    void setProfile (PerformanceProfile p) noexcept { profile_.store (p, std::memory_order_relaxed); }
    PerformanceProfile profile() const noexcept { return profile_.load (std::memory_order_relaxed); }
    // Compensation de latence (profil Studio) : thread de message, à rappeler régulièrement et après chaque changement d'insert ou de routage.
    // Recalcule la latence de chaque chemin (inserts, bus, master), aligne par des retards les signaux qui se somment et les sorties, publie le plan s'il change.
    // Rend la latence totale (échantillons) à annoncer à l'hôte : 0 en profil Live (les modules passent alors à latence nulle).
    int updateLatencyPlan();
    int latencySamples() const noexcept { return planTotal_.load (std::memory_order_relaxed); }

    // Paramètres lus à chaque bloc (SamplerParams.h, ProbePool.h, Mixer.h). Appelé hors temps réel, avant l'audio.
    void attachParameters (const ParameterSet* parameters) noexcept;
    void publishModulation (int track);   // thread de message : résout la matrice de la piste sur le jeu de paramètres

    // Instrument d'une piste : thread de message. L'audio l'adopte au début du bloc suivant ; les voix en cours s'arrêtent.
    // Démarre le thread disque (et ses tampons) la première fois qu'un instrument en streaming est publié.
    void publishInstrument (int track, std::shared_ptr<const Instrument> instrument);
    // Écoute libre : l'objet est remis au thread audio ; l'appelant doit le garder vivant quelques écoutes de plus (les voix y pointent), puis poster UiEvent::PreviewSample.
    void publishPreview (int track, std::shared_ptr<const PreviewSample> sample);
    void publishInstrument (std::shared_ptr<const Instrument> instrument) { publishInstrument (0, std::move (instrument)); }
    void collectGarbage(); // thread de message, régulièrement : détruit ce que le thread audio a rendu

    // Insert d'un emplacement (piste, bus ou Master ; `index` ignoré pour le Master ; `slot` 0..7) : thread de message.
    // Le module doit être préparé (createInsert avec sampleRate() / maxBlockSize()). Nul : l'emplacement devient vide.
    // L'audio l'adopte au début du bloc suivant ; le thread audio en devient le seul utilisateur du DSP.
    void setInsert (StripKind kind, int index, int slot, std::shared_ptr<const InsertInstance> insert);
    int maxBlockSize() const noexcept { return maxBlockSize_; }

    // Type de génération d'une piste (Sampler / Synthé) et patch du synthé : thread de message, pris en compte au bloc suivant.
    // Un changement de type coupe net les voix de la piste.
    void setTrackType (int track, TrackType type) noexcept;
    void publishSynth (int track, std::shared_ptr<const SynthPatch> patch);
    // Module FM : description d'une piste de type FM (adoptée au bloc suivant).
    void publishFm (int track, std::shared_ptr<const fm::FmDescription> description);
    // Type Orgue : description des claviers (canaux, split, transposition), adoptée au bloc suivant.
    void publishOrgan (int track, std::shared_ptr<const organ::OrganDescription> description);
    // Filtre MIDI avancé d'une piste (normalement publié par setTrackSettings ; direct pour les tests).
    void setTrackMidiFilter (int track, std::shared_ptr<const midi::CompiledFilter>);
    // Un message de contrôleur (CC, pitch bend, pression, programme) : retenu pour les conditions MIDI des zones. Thread audio.
    void midiController (const midi::Message& m) noexcept { midiState_.update (m); }
    // Molette de hauteur de l'hôte sur `channel` (0..15), 14 bits (0..16383, repos 8192) : la matrice et les conditions MIDI n'en voient que 7 (midiController) ;
    // les pistes Synthé et FM suivent celle-ci. Thread audio.
    void pitchBendMidi (int channel, int value14) noexcept { bend14_[static_cast<std::size_t> (channel & 15)].v.store (value14 < 0 ? 0 : value14 > 16383 ? 16383 : value14, std::memory_order_relaxed); }
    // Molettes de l'écran, pour une piste Synthé ou FM (thread de message) : hauteur -1..+1 (au repos, la molette de l'hôte reprend la main), modulation 0..1.
    void setUiPitchBend (int track, float position) noexcept;
    void setUiModWheel (int track, float position) noexcept;
    // Écrit la valeur (0..100) d'un modulateur quand une touche de keyswitch le bascule ; appelé par le thread audio (fourni par le processeur, qui relaie vers l'hôte).
    void setModulatorWriter (std::function<void (ParamId, double)> w) { modulatorWriter_ = std::move (w); }
    // Notes en cours d'une piste Synthé figées par un changement de preset (capture par voix).
    int frozenVoices (int track) const noexcept
    {
        return track < 0 || track >= kTracks ? 0 : tracks_[static_cast<std::size_t> (track)].synth.frozenVoices();
    }
    // Piste active : une piste inactive ne reçoit aucune note et ne produit rien (le mixeur ne l'affiche pas). Par défaut, toutes.
    void setTrackActive (int track, bool active) noexcept;

    // Réglages non automatisables du mixeur (routage, canal MIDI, mode de pan...) : thread de message, pris en compte au bloc suivant.
    void setTrackSettings (int track, const mixer::StripSettings&); // construit le filtre MIDI : thread de message
    void setBusSettings (int bus, const mixer::StripSettings&) noexcept;
    // Pistes parallèles : `parent` 0..15 = piste, 16..47 = piste parallèle ; `tap` : point de captation dans la chaîne du parent.
    // Une piste parallèle dont le parent est inactif ou qui forme une boucle n'est jamais traitée.
    // `followParentVolume` : le gain de fader du parent s'applique aussi à cette piste (captation avant fader seulement).
    void setParallel (int p, bool active, int parent, mixer::TapPoint tap, bool followParentVolume = false) noexcept;
    void setParallelSettings (int p, const mixer::StripSettings&) noexcept;
    void setMasterSettings (const mixer::MasterSettings&) noexcept;

    // Événements venus de l'interface (clavier de la matrice, bouton d'écoute des zones). Thread de message UNIQUEMENT (un seul
    // producteur) ; le thread audio les applique au début du bloc suivant. Rend false si la file est pleine.
    struct UiEvent
    {
        enum class Type : std::uint8_t { NoteOn, NoteOff, PreviewZone, PreviewRelease, PreviewStop, Panic, ReleaseAll, PreviewSample, KeyboardOn, KeyboardOff } type = Type::NoteOn;
        int channel = 0, key = 0;      // NoteOn / NoteOff ; PreviewZone : `key` = indice de zone
        float velocity = 0.0f;
        int track = 0;                 // piste visée (NoteOn, NoteOff, Preview*) ; Panic / ReleaseAll : toutes
        std::int64_t frame = -1;       // PreviewZone : point de départ dans le fichier (-1 : le début de la région jouée)
        std::uint8_t source = 0;       // PreviewZone : SamplerVoices::PreviewSpec::Source
        std::uint8_t loopIndex = 0;    // PreviewZone : boucle visée (LoopOnce / LoopHeard)
    };
    bool postUiEvent (const UiEvent&) noexcept;

    // Notes jouées, pour l'interface (curseur de vélocité de la matrice) : piste, touche et vélocité 1..127 APRÈS la courbe de
    // l'instrument (celle qui choisit la couche). Producteur : thread audio ; consommateur : thread de message.
    struct NoteTrace { int track = 0, key = 0, velocity = 0; };
    bool popNoteTrace (NoteTrace&) noexcept;

    // Voix qui sonnent, pour le moniteur de l'interface : copie cohérente du dernier bloc (verrou de séquence : le thread audio
    // n'attend jamais). Thread de message ; rend le nombre de voix copiées.
    using VoiceInfo = SamplerVoices::VoiceInfo;
    static constexpr int kVoiceSnapshot = 512;
    int readVoices (VoiceInfo* out, int maxOut) const noexcept; // VoiceInfo::track est rempli
    // Le moniteur n'est alimenté que lorsqu'il est demandé (thread de message) : éteint, le thread audio ne publie rien.
    void setVoiceMonitor (bool on) noexcept { voiceMonitor_.store (on, std::memory_order_relaxed); if (! on) voiceCount_.store (0, std::memory_order_relaxed); }
    bool voiceMonitor() const noexcept { return voiceMonitor_.load (std::memory_order_relaxed); }

    // Niveaux de crête du dernier bloc (0..~1), pour les vu-mètres ; thread de message.
    struct Meter { float peakL = 0.0f, peakR = 0.0f, rmsL = 0.0f, rmsR = 0.0f; };
    Meter trackMeter (int track) const noexcept;
    Meter busMeter (int bus) const noexcept;
    Meter parallelMeter (int p) const noexcept;
    Meter masterMeter() const noexcept;
    // Sonie du Master (LUFS momentanée, à court terme, intégrée, crête vraie) ; mesurée par libebur128 sur un thread d'analyse.
    LoudnessMeter::Reading masterLoudness() const noexcept { return loudness_.read(); }
    void resetLoudness() noexcept { loudness_.reset(); }

    int activeVoices() const noexcept;
    // Nombre de voix actives de toutes les pistes, publié par le thread audio à chaque bloc (lisible depuis l'interface sans verrou).
    // Touches tenues d'une piste (mot 0 : notes 0..63, mot 1 : 64..127), publiées par le thread audio.
    // Touches tenues d'une piste sur un canal MIDI (0..15) : bits 0..63 (word 0) et 64..127 (word 1).
    std::uint64_t heldChannelWord (int track, int channel, int word) const noexcept
    {
        if (track < 0 || track >= kTracks || word < 0 || word > 1) return 0;
        return tracks_[static_cast<std::size_t> (track)].heldChannel[channel & 15][word].load (std::memory_order_relaxed);
    }
    // Touches tenues sur un canal MIDI, toutes pistes confondues (voyants et touches allumées d'une Façade qui n'est pas celle de la piste jouée : console d'orgue).
    std::uint64_t heldChannelWordAny (int channel, int word) const noexcept
    {
        if (word < 0 || word > 1) return 0;
        std::uint64_t bits = 0;
        for (const auto& t : tracks_) bits |= t.heldChannel[channel & 15][word].load (std::memory_order_relaxed);
        return bits;
    }
    std::uint64_t heldKeyWord (int track, int word) const noexcept
    {
        if (track < 0 || track >= kTracks || word < 0 || word > 1) return 0;
        return tracks_[static_cast<std::size_t> (track)].held[word].load (std::memory_order_relaxed);
    }
    int totalActiveVoices() const noexcept { return totalVoices_.load (std::memory_order_relaxed); }
    std::uint32_t notesIn (int track) const noexcept { return tracks_[static_cast<std::size_t> (track)].notesIn.load (std::memory_order_relaxed); }
    int modsOn (int track) const noexcept { return tracks_[static_cast<std::size_t> (track)].voices.modsOn(); }
    int lastPicks (int track) const noexcept { return tracks_[static_cast<std::size_t> (track)].voices.lastPicks(); }
    int lastKeyZones (int track) const noexcept { return tracks_[static_cast<std::size_t> (track)].voices.lastKeyZones(); }
    int adoptedZones (int track) const noexcept { return tracks_[static_cast<std::size_t> (track)].zones.load (std::memory_order_relaxed); }
    int activeVoices (int track) const noexcept
    {
        const auto& t = tracks_[static_cast<std::size_t> (track)];
        return t.voices.activeVoices() + t.synth.activeVoices() + t.fm.activeVoices() + t.organ.heldKeys();
    }
    double sampleRate() const noexcept { return sampleRate_; }
    // Sous-alimentations du streaming depuis le démarrage (une voix s'est éteinte faute de données du disque).
    std::uint32_t streamUnderruns() const noexcept { return streamer_.underruns(); }
    int activeNotes() const noexcept { return activeNotes_; }

private:
    struct AtomicSend
    {
        std::atomic<int> destination { 0 }, tap { 3 }, mute { 0 };
        std::atomic<float> pan { 0.0f };
    };
    struct AtomicStrip
    {
        std::atomic<int> midiChannel { 0 };
        std::atomic<unsigned> midiMask { 0 };   // plusieurs canaux (voir StripSettings::midiChannelMask) ; 0 : voir midiChannel
        std::atomic<int> mainDest { 0 }, mainEnabled { 1 }, directOut { 0 }, panMode { 0 }, hostInput { 1 };
        std::array<AtomicSend, mixer::numSends> sends;
        void load (const mixer::StripSettings&) noexcept;
    };
    struct ParamIndices
    {
        static constexpr std::size_t N = ParameterSet::npos;
        std::size_t volume = N, pan = N, mute = N, solo = N;
        std::size_t sendLevel[mixer::numSends];
        std::size_t sampler[mixer::kSamplerKinds];
        std::size_t synth[mixer::kSynthKinds];
        std::size_t synthEnv[mixer::kSynthEnvKinds];
        std::size_t fm[mixer::kFmKinds];
        std::size_t organ[mixer::kOrganKinds];
        std::size_t slot[16];
        std::size_t mod[kModulators];
        std::size_t fmExtra[mixer::kFmExtraKinds];
        ParamIndices() noexcept { for (auto& v : sendLevel) v = N; for (auto& v : sampler) v = N; for (auto& v : synth) v = N; for (auto& v : fm) v = N; for (auto& v : organ) v = N; for (auto& v : slot) v = N; }
    };
    // Un emplacement d'insert : l'instance (remise au thread audio) et l'index de son paramètre de bypass (celui de l'hôte).
    struct InsertSlot
    {
        HandoffSlot<InsertInstance> slot;
        std::size_t bypass = ParameterSet::npos;
    };
    using Inserts = std::array<InsertSlot, mixer::numInserts>;

    // Routeur de notes : règles résolues (remises au thread audio) et, pour chaque note en cours, où elle a été envoyée (le Note Off défait la même chose).
    HandoffSlot<router::Compiled> routerSlot_;
    const router::Compiled* router_ = nullptr;      // thread audio
    router::Config routerConfig_;                   // thread de message : ce qui est publié
    // direct : la première destination est la note d'origine d'un clavier de l'interface, jouée directement sur la piste `originTrack` (sans compteur)
    struct Routed { std::uint8_t count = 0; bool direct = false; std::int8_t originTrack = -1; std::array<std::uint8_t, router::kMaxTargets> channel {}, key {}; };
    std::array<Routed, 16 * 128> routed_ {};        // par (canal, touche) jouée
    std::array<std::uint8_t, 16 * 128> routeCount_ {};   // par (canal, touche) destinataire : nombre de sources qui la tiennent
    void dispatchNoteOn (int channel, int key, float velocity) noexcept;
    void dispatchNoteOff (int channel, int key) noexcept;
    void releaseRouted (int slot) noexcept;
    void clearRouted() noexcept { routed_.fill (Routed {}); routeCount_.fill (0); clearDynamic(); }

    // Accouplements de basse et de mélodie : la note accouplée est celle de la touche tenue la plus grave (ou la plus aiguë) du clavier source, et change au fil des notes.
    // `sourceHeld_` : les touches tenues de chaque canal (bit de la touche) ; `dynamic_` : par règle dynamique, la destination actuellement tenue (-1 : aucune).
    struct DynamicTarget { int channel = -1, key = -1; };
    std::array<std::array<std::uint64_t, 2>, 16> sourceHeld_ {};
    std::array<float, 16 * 128> sourceVelocity_ {};
    std::array<DynamicTarget, router::kMaxRules> dynamic_ {};
    void trackSource (int channel, int key, float velocity, bool down) noexcept;   // note la touche tenue ou relâchée, puis met les accouplements à jour
    void refreshDynamic() noexcept;                                               // recalcule la note accouplée de chaque règle de basse / mélodie
    void releaseDynamic() noexcept;                                               // relâche toutes les notes accouplées (changement de règles)
    void clearDynamic() noexcept { sourceHeld_ = {}; dynamic_.fill (DynamicTarget {}); }   // panique / tout relâcher : les voix sont déjà coupées

    struct Track
    {
        Inserts inserts;
        InstrumentSlot slot;
        SamplerVoices voices;
        HandoffSlot<PreviewSample> previewSlot;         // écoute libre
        HandoffSlot<midi::CompiledFilter> filterSlot;   // filtre MIDI avancé de la piste
        const midi::CompiledFilter* filter = nullptr;   // thread audio
        midi::FilterDescription filterDescription;      // thread de message : ce qui est publié
        HandoffSlot<modulation::Compiled> modSlot;      // matrice de modulation résolue (indices de paramètres)
        modulation::Matrix modDescription;              // thread de message : ce qui est publié
        modulation::Modulator modulator;                // thread audio : LFO, enveloppes, sources MIDI
        HandoffSlot<VelocityLut> velocitySlot;          // courbe de transfert de la vélocité reçue (aucune : vélocité telle quelle)
        const VelocityLut* velocityLut = nullptr;       // thread audio
        VelocityCurve velocityDescription;              // thread de message : ce qui est publié
        std::array<std::uint64_t, 32> acceptedNotes {}; // thread audio : notes dont le Note On a passé (16 canaux x 128)
        HandoffSlot<fm::FmDescription> fmSlot;         // module FM : description (patch DX7 et réglages de voix)
        fm::FmVoices fm;
        HandoffSlot<organ::OrganDescription> organSlot; // type Orgue : description des claviers
        organ::OrganVoices organ;
        HandoffSlot<SynthPatch> synthSlot;
        SynthPatchKeeper synthKeeper; // thread de message : anciens patchs utilisés par des voix figées
        SynthVoices synth;
        std::atomic<std::uint64_t> held[2] {}; // touches tenues (bits 0..127), pour l'affichage du clavier
        std::atomic<std::uint64_t> heldChannel[16][2] {}; // idem par canal MIDI (0..15) : un clavier de la Façade n'allume que son canal
        std::atomic<int> type { 0 };   // TrackType
        std::atomic<int> active { 1 };
        std::atomic<std::uint32_t> notesIn { 0 };   // notes reçues par la piste depuis le début (voyant d'activité)
        std::atomic<int> zones { 0 };               // zones de l'instrument adopté par le fil audio (0 : aucun)
        int appliedType = 0, appliedActive = 1; // thread audio : ce qui a déjà été pris en compte
        AtomicStrip config;
        ParamIndices idx;
        float lastGain = 1.0f;                      // gain de l'instrument (lissé)
        float lastFader = 1.0f, lastPanL = 1.0f, lastPanR = 1.0f;
        std::array<float, mixer::numSends> lastSend {};
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f }, rmsL { 0.0f }, rmsR { 0.0f };
    };
    struct Parallel
    {
        Inserts inserts;
        AtomicStrip config;
        ParamIndices idx;
        std::atomic<int> active { 0 }, parent { 0 }, tap { 3 }, follow { 0 };
        float lastFader = 1.0f, lastPanL = 1.0f, lastPanR = 1.0f;
        std::array<float, mixer::numSends> lastSend {};
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f }, rmsL { 0.0f }, rmsR { 0.0f };
    };
    struct Bus
    {
        Inserts inserts;
        AtomicStrip config;
        ParamIndices idx;
        float lastFader = 1.0f, lastPanL = 1.0f, lastPanR = 1.0f;
        std::array<float, mixer::numSends> lastSend {};
        bool hasInput = false;
        std::atomic<float> peakL { 0.0f }, peakR { 0.0f }, rmsL { 0.0f }, rmsR { 0.0f };
    };

    void processChunk (float* const* out, int numChannels, int frames) noexcept;
    void drainUiEvents() noexcept;
    struct Stereo;
    struct Level;
    void adoptInserts() noexcept;
    // --- traitement d'une bande (un bloc de `chunkN_` échantillons)
    bool runInserts (Inserts&, Stereo&) noexcept;
    static bool hasInserts (const Inserts&) noexcept;
    static bool hasSourceInsert (const Inserts&) noexcept;   // un générateur (Diapason) : la bande doit tourner sans instrument ni entrée
    void applyFaderPan (const Stereo& in, Stereo& postFader, Stereo& fin, const AtomicStrip&, const ParamIndices&, float& lastFader,
                        float& lastPanL, float& lastPanR, bool muted) noexcept;
    void meterOf (const Stereo& out, std::atomic<float>& pkL, std::atomic<float>& pkR, std::atomic<float>& rmsL, std::atomic<float>& rmsR) noexcept;
    void routeStrip (const AtomicStrip&, const ParamIndices&, std::array<float, mixer::numSends>& lastSend, const Stereo& pre, const Stereo& post,
                     const Stereo& postFader, const Stereo& out, bool isBus, int busIndex, bool sumIntoParent, int stripId) noexcept;
    // Retard de compensation d'une arête (bande, arête) : rend `src` tel quel s'il n'y en a pas, sinon une copie retardée (tampon commun, valable jusqu'à l'appel suivant).
    const Stereo& delayed (int strip, int edge, const Stereo& src) noexcept;
    void processParallel (int p, int depth, bool ancestorSolo, const Stereo& parentPre, const Stereo& parentPost, const Stereo& parentFader,
                          const Stereo& parentFin, Stereo& parentSum, float parentGainFrom, float parentGainTo) noexcept;
    std::pair<float*, float*> channelPair (int output) const noexcept;
    Inserts* insertsOf (StripKind, int index) noexcept;
    void pruneSynthKeeper (Track&);
    void enforceGlobalVoiceLimit() noexcept; // avant un Note On : vole la voix la plus faible du plugin si la limite globale est atteinte
    void pushTrace (int track, int key, float shapedVelocity) noexcept;
    void publishVoices() noexcept;
    double param (std::size_t index, double fallback) const noexcept
    {
        return params_ != nullptr && index != ParameterSet::npos ? params_->effectiveAt (index) : fallback;   // avec la modulation
    }

    double sampleRate_ = 0.0;
    int maxBlockSize_ = 0;
    int activeNotes_ = 0;
    std::atomic<PerformanceProfile> profile_ { PerformanceProfile::Live };

    static constexpr std::size_t kUiQueueSize = 256; // puissance de 2
    std::array<UiEvent, kUiQueueSize> uiQueue_ {};
    std::atomic<std::size_t> uiHead_ { 0 }, uiTail_ { 0 }; // écrit par l'interface / par l'audio
    // instantané des voix (2 mots de 64 bits par voix), protégé par un compteur de séquence (impair = écriture en cours)
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t> (kVoiceSnapshot) * 2u> voiceWords_ {};
    std::atomic<std::uint32_t> voiceSeq_ { 0 };
    std::atomic<int> voiceCount_ { 0 };
    std::atomic<int> totalVoices_ { 0 };
    std::atomic<bool> voiceMonitor_ { false };
    static constexpr std::size_t kTraceSize = 64;           // puissance de 2
    std::array<std::atomic<std::uint32_t>, kTraceSize> trace_ {};
    std::atomic<std::size_t> traceHead_ { 0 }, traceTail_ { 0 };

    DiskStreamer streamer_;
    std::array<Track, kTracks> tracks_;
    std::array<Bus, kBuses> buses_;
    std::atomic<int> masterOutput_ { 1 }, masterPanMode_ { 0 };
    LoudnessMeter loudness_;
    std::vector<float> loudL_, loudR_; // le signal du Master tel qu'envoyé (après trim, inserts, fader, pan, mono)
    std::atomic<int> globalMaxVoices_ { mixer::MasterSettings::defaultMaxVoices };
    const ParameterSet* params_ = nullptr;
    // Décalages de modulation écrits dans des modules en insert : (piste, emplacement, instance, indice du paramètre du module). L'instance sert
    // seulement de repère (jamais déréférencée au reset) : si l'emplacement en porte une autre, le décalage ne concerne plus personne.
    struct InsertTouch { int track; int slot; const InsertInstance* instance; std::size_t index; };
    std::vector<InsertTouch> insertTouched_;
    struct InsertSinkContext { Engine* engine; int track; };
    static void addInsertOffset (void* context, int slot, ParamId moduleParam, float delta) noexcept;
    static constexpr int kModulationChunk = 64;
    std::atomic<bool> modulationActive_ { false };   // au moins une piste a des routes de modulation
    std::vector<std::size_t> modTouched_;   // paramètres dont le décalage de modulation est non nul (remis à zéro au bloc suivant)
    std::size_t masterVolume_ = ParameterSet::npos, masterPan_ = ParameterSet::npos, masterMute_ = ParameterSet::npos,
                masterTrim_ = ParameterSet::npos, masterMono_ = ParameterSet::npos;
    Inserts masterInserts_;
    // ---- compensation de latence
    static constexpr int kEdgeSelf = mixer::numSends + 3;   // arêtes d'une bande : envois 0..numSends-1, principale, directe, vers la somme du parent, elle-même vers sa somme
    static constexpr int kEdgeMain = mixer::numSends, kEdgeDirect = mixer::numSends + 1, kEdgeInto = mixer::numSends + 2, kEdges = mixer::numSends + 4;
    static constexpr int kStrips = kTracks + kBuses + kParallels + 1;   // pistes, bus, pistes parallèles, Master
    static constexpr int masterStrip = kTracks + kBuses + kParallels;
    struct LatencyPlan
    {
        struct Line { std::vector<float> l, r; std::size_t w = 0; int delay = 0; std::uint32_t stamp = 0; };
        int total = 0;
        int used = 0;   // arêtes retardées : 0 = plan vide, le thread audio ne le consulte pas
        std::vector<int> usedIndex;   // leurs indices (bande * kEdges + arête), pour le balayage de fin de bloc
        std::vector<int> delays = std::vector<int> (static_cast<std::size_t> (kStrips * kEdges), 0);
        std::vector<Line> lines = std::vector<Line> (static_cast<std::size_t> (kStrips * kEdges));   // à l'usage du seul thread audio une fois publié
    };
    HandoffSlot<LatencyPlan> planSlot_;
    LatencyPlan* plan_ = nullptr;                     // thread audio
    std::uint32_t planStamp_ = 0;                      // thread audio : numéro du bloc
    std::vector<int> publishedDelays_;                 // thread de message : le dernier plan publié
    int publishedTotal_ = -1;
    bool publishedEmpty_ = false;                      // le plan publié n'a aucune arête retardée
    std::vector<int> workDelays_;                      // tampon de calcul de updateLatencyPlan (réutilisé, sans allocation à chaque appel)
    std::atomic<int> planTotal_ { 0 };
    std::array<std::array<std::shared_ptr<const InsertInstance>, mixer::numInserts>, kStrips> msgInserts_;   // thread de message : les inserts, pour en lire la latence
    static int stripIndex (StripKind kind, int index) noexcept
    {
        return kind == StripKind::Track ? index : kind == StripKind::Bus ? kTracks + index : kind == StripKind::Parallel ? kTracks + kBuses + index : masterStrip;
    }
    void feedSilence (LatencyPlan::Line&, int n) noexcept;
    std::atomic<bool> insertsDirty_ { false }; // un insert a été publié : le thread audio les adopte au prochain bloc
    float lastMasterTrim_ = 1.0f, lastMasterFader_ = 1.0f, lastMasterPanL_ = 1.0f, lastMasterPanR_ = 1.0f;
    std::atomic<float> masterPeakL_ { 0.0f }, masterPeakR_ { 0.0f }, masterRmsL_ { 0.0f }, masterRmsR_ { 0.0f };

    // tampons de mixage (alloués dans prepare)
    struct Stereo { std::vector<float> l, r; void resize (std::size_t n) { l.assign (n, 0.0f); r.assign (n, 0.0f); } };
    struct Level { Stereo pre, post, fader, fin, sum; void resize (std::size_t n) { pre.resize (n); post.resize (n); fader.resize (n); fin.resize (n); sum.resize (n); } };
    Stereo raw_, inserted_, postFader_, final_, sum0_, master_; // niveau 0 : la piste
    Stereo delayTmp_;                                           // sortie d'un retard de compensation
    std::vector<Level> levels_;                                 // niveau d >= 1 : levels_[d - 1], pour les pistes parallèles
    std::array<Stereo, kBuses> busIn_;
    std::array<Stereo, kTracks> trackIn_;                       // envois reçus par les pistes « Entrée audio »
    std::array<bool, kTracks> trackHasInput_ {};
    std::array<bool, kTracks> audioInNow_ {};                   // types lus une fois par bloc (un changement de type en cours de bloc ne double rien)
    const float *hostL_ = nullptr, *hostR_ = nullptr;           // entrée de l'hôte (appel en cours)
    int hostFrames_ = 0;
    const float *chunkHostL_ = nullptr, *chunkHostR_ = nullptr; // idem, décalée au bloc en cours
    int currentRoot_ = -1;                                      // piste en cours de traitement (ses pistes parallèles comprises)
    std::function<void (ParamId, double)> modulatorWriter_;
    void keyswitch (int track, int channel, int key, bool down) noexcept;
    midi::State midiState_;   // dernier état des contrôleurs de chaque canal (conditions MIDI des zones)
    struct Wheel { std::atomic<int> v { 8192 }; };   // 14 bits, repos au milieu
    std::array<Wheel, 16> bend14_;                   // molette de hauteur de l'hôte, par canal
    std::array<Wheel, 16> uiBend_;                   // molette de hauteur de l'écran, par piste
    std::array<std::atomic<int>, 16> uiMod_ {};      // molette de modulation de l'écran, par piste (0..127)
    double trackBend (int track) const noexcept;     // -1..+1 : celle de l'écran si elle n'est pas au repos, sinon la plus écartée des canaux que la piste écoute
    int trackModWheel (int track) const noexcept;    // 0..127 : le plus fort du CC 1 des canaux écoutés et de la molette de l'écran
    bool currentRootAudioIn_ = false;
    std::array<Parallel, kParallels> parallels_;
    // contexte du bloc en cours
    float* const* chunkOut_ = nullptr;
    int chunkChannels_ = 0, chunkN_ = 0;
    bool anySolo_ = false;
    bool busSolo_ = false;
    std::array<int, mixer::numTracks + mixer::numParallels> firstChild_ {};
    std::array<int, mixer::numParallels> nextSibling_ {};
    std::array<unsigned char, mixer::numTracks + mixer::numParallels> subtreeSolo_ {}, subtreeBusy_ {};
};
} // namespace neptune::core
