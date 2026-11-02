#include <algorithm>
#include "PluginProcessor.h"
#include "neptune/core/MemoryBudget.h"
#include "FacadeBundle.h"
#include "neptune/core/GlobalPreset.h"
#include "neptune/core/CompressorModule.h"
#include "neptune/core/OverdriveModule.h"
#include "NeptuneParameter.h"
#include "PluginEditor.h"
#include "neptune/core/FmPreset.h"
#include "neptune/core/InstrumentEdit.h"
#include "neptune/core/MappingEdit.h"
#include "neptune/core/OrganPreset.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/ProbePool.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/State.h"
#include "neptune/core/Synth.h"
#include <set>

#ifndef NEPTUNE_PROBE_TRACKS
 #define NEPTUNE_PROBE_TRACKS 16
#endif

// Nombre de sorties stéréo déclarées à l'hôte (sonde du point 4 : 32 depuis 2026-10-15).
#ifndef NEPTUNE_PROBE_OUTPUTS
 #define NEPTUNE_PROBE_OUTPUTS 32
#endif

namespace
{
// Dossier de configuration utilisateur de Neptune : ~/.config/Neptune, %APPDATA%\\Neptune, ~/Library/Application Support/Neptune.
std::filesystem::path configDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return std::filesystem::u8path (base.getChildFile ("Neptune").getFullPathName().toStdString());
}
}

// Thread de chargement des samples : construit les instruments (lit le disque), les publie au moteur piste par piste, et
// libère les instruments rendus par le thread audio. Seul consommateur de la file de collecte (InstrumentSlot::collect).
// Le cache de fichiers est PARTAGÉ entre les pistes : deux pistes qui jouent les mêmes fichiers n'en gardent qu'une copie.
class NeptuneProcessor::InstrumentLoader : public juce::Thread
{
public:
    explicit InstrumentLoader (NeptuneProcessor& owner) : juce::Thread ("Neptune sample loader"), owner_ (owner) { startThread(); }
    ~InstrumentLoader() override { stopThread (15000); }

    void request (int track, neptune::core::InstrumentDescription d, bool force)
    {
        {
            const juce::ScopedLock lock (lock_);
            auto& r = pending_[static_cast<std::size_t> (track)];
            r.description = std::move (d);
            r.has = true;
            r.force = r.force || force;
        }
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (500);
            owner_.engine_.collectGarbage();

            int track = -1;
            neptune::core::InstrumentDescription d;
            bool force = false;
            {
                const juce::ScopedLock lock (lock_);
                for (std::size_t t = 0; t < pending_.size(); ++t)
                    if (pending_[t].has)
                    {
                        track = static_cast<int> (t);
                        d = std::move (pending_[t].description);
                        force = pending_[t].force;
                        pending_[t].has = false;
                        pending_[t].force = false;
                        break;
                    }
            }
            if (track < 0) continue;
            buildAndPublish (track, std::move (d), force);
            if (hasAnyPending()) notify(); // d'autres pistes attendent : pas d'attente de 500 ms
        }
    }

private:
    struct Request
    {
        neptune::core::InstrumentDescription description;
        bool has = false, force = false;
    };

    void buildAndPublish (int track, neptune::core::InstrumentDescription d, bool force)
    {
        if (force) cache_.clear();
        InstrumentStatus st;
        if (d.empty())
        {
            owner_.engine_.publishInstrument (track, std::make_shared<neptune::core::Instrument>());
            owner_.setStatus (track, st); // None
            return;
        }

        st.kind = InstrumentStatus::Kind::Loading;
        st.total = static_cast<int> (d.zones.size());
        owner_.setStatus (track, st);
        std::vector<std::string> warnings;
        // Fichiers des AUTRES pistes : à conserver dans le cache partagé (buildInstrument purge ce qui n'est pas dans sa description)
        std::set<std::string> keep;
        {
            const auto others = owner_.otherTracksPaths (track);
            keep.insert (others.begin(), others.end());
        }
        // Abandonne dès qu'une nouvelle demande arrive pour CETTE piste (le cache garde ce qui est déjà lu) ou à l'arrêt du plugin.
        auto inst = neptune::core::buildInstrument (d, &warnings, &cache_, [&] (int done, int total)
        {
            if (threadShouldExit() || hasPendingFor (track)) return false;
            if (done % 16 == 0)
            {
                InstrumentStatus p;
                p.kind = InstrumentStatus::Kind::Loading;
                p.done = done;
                p.total = total;
                owner_.setStatus (track, std::move (p));
            }
            return true;
        }, &keep);
        if (threadShouldExit() || inst == nullptr) return; // annulé : la demande suivante est traitée au tour d'après

        owner_.engine_.publishInstrument (track, inst);
        st = {};
        if (! inst->zones.empty())
        {
            const auto& sd = *inst->zones.front().sample;
            st.kind = InstrumentStatus::Kind::Loaded;
            st.name = sd.name;
            st.channels = sd.channels;
            st.bitDepth = sd.sourceBitDepth;
            st.sampleRate = sd.sampleRate;
            st.seconds = sd.durationSeconds();
            st.zones = static_cast<int> (inst->zones.size());
            std::set<const neptune::core::SampleData*> distinct;
            double bytes = 0.0;
            for (const auto& z : inst->zones)
                if (distinct.insert (z.sample.get()).second) bytes += static_cast<double> (z.sample->bytes());
            st.files = static_cast<int> (distinct.size());
            st.megabytes = bytes / (1024.0 * 1024.0);
        }
        else
        {
            st.kind = InstrumentStatus::Kind::Error;
            st.error = warnings.empty() ? "no usable zone" : warnings.front();
        }
        owner_.setStatus (track, st);
    }

    bool hasPendingFor (int track)
    {
        const juce::ScopedLock lock (lock_);
        return pending_[static_cast<std::size_t> (track)].has;
    }

    bool hasAnyPending()
    {
        const juce::ScopedLock lock (lock_);
        for (const auto& r : pending_)
            if (r.has) return true;
        return false;
    }

    NeptuneProcessor& owner_;
    juce::CriticalSection lock_;
    std::array<Request, neptune::core::mixer::numTracks> pending_;
    neptune::core::SampleCache cache_; // thread du chargeur uniquement, partagé par toutes les pistes
};

neptune::core::InstrumentDescription NeptuneProcessor::instrumentDescription (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))].instrument;
}

// Chemins des fichiers que les AUTRES pistes ont besoin de garder en mémoire (cache partagé du chargeur).
std::set<std::string> NeptuneProcessor::otherTracksPaths (int track) const
{
    std::set<std::string> paths;
    const juce::ScopedLock lock (instrumentLock_);
    for (std::size_t t = 0; t < mixer_.tracks.size(); ++t)
    {
        if (static_cast<int> (t) == track) continue;
        const auto& d = mixer_.tracks[t].instrument;
        const auto mask = neptune::core::loadedZones (d);
        for (std::size_t i = 0; i < d.zones.size() && i < mask.size(); ++i)
            if (mask[i]) paths.insert (d.zones[i].samplePath);
    }
    return paths;
}

NeptuneProcessor::InstrumentStatus NeptuneProcessor::instrumentStatus (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return status_[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))];
}

std::string NeptuneProcessor::loadSummary() const
{
    std::string out;
    const juce::ScopedLock lock (instrumentLock_);
    for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
    {
        const auto& track = mixer_.tracks[static_cast<std::size_t> (t)];
        if (! track.active) continue;
        const auto& st = status_[static_cast<std::size_t> (t)];
        const std::string name = track.strip.name.empty() ? "Track " + std::to_string (t + 1) : track.strip.name;
        if (st.kind == InstrumentStatus::Kind::Loading) out += (out.empty() ? "" : "   |   ") + std::string ("Loading ") + name + " " + std::to_string (st.done) + "/" + std::to_string (st.total);
        else if (st.kind == InstrumentStatus::Kind::Error) out += (out.empty() ? "" : "   |   ") + name + ": " + st.error;
    }
    return out;
}

std::string NeptuneProcessor::activitySummary() const
{
    std::string out;
    const juce::ScopedLock lock (instrumentLock_);
    for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
    {
        const auto& track = mixer_.tracks[static_cast<std::size_t> (t)];
        if (! track.active || track.type != neptune::core::TrackType::Sampler || track.instrument.empty()) continue;
        const std::string name = track.strip.name.empty() ? "T" + std::to_string (t + 1) : track.strip.name;
        out += (out.empty() ? "" : "  |  ") + name + " ch" + std::to_string (track.strip.midiChannel) + ": " + std::to_string (engine_.adoptedZones (t)) + " zones, "
             + std::to_string (engine_.notesIn (t)) + " notes, " + std::to_string (engine_.activeVoices (t)) + " voices, "
             + std::to_string (engine_.modsOn (t)) + " mods on, last note " + std::to_string (engine_.lastPicks (t)) + "/" + std::to_string (engine_.lastKeyZones (t));
    }
    return out;
}

void NeptuneProcessor::setStatus (int track, InstrumentStatus s)
{
    const juce::ScopedLock lock (instrumentLock_);
    status_[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))] = std::move (s);
}

void NeptuneProcessor::setInstrumentDescription (int track, neptune::core::InstrumentDescription d, bool forceReload)
{
    track = juce::jlimit (0, neptune::core::mixer::numTracks - 1, track);
    for (auto& z : d.zones) z = neptune::core::sanitised (z);
    {
        const juce::ScopedLock lock (instrumentLock_);
        auto& cur = mixer_.tracks[static_cast<std::size_t> (track)].instrument;
        if (d == cur && ! forceReload) return;
        cur = d;
    }
    ++revision_;
    loader_->request (track, std::move (d), forceReload);
    notifyStateChanged(); // la description fait partie de l'état du projet
}

void NeptuneProcessor::uiEditInstrument (neptune::core::InstrumentDescription d, std::string name, std::string mergeKey)
{
    uiEditInstrument (selectedTrack(), std::move (d), std::move (name), std::move (mergeKey));
}

neptune::core::ParamId NeptuneProcessor::ensureGroupSwitch (int track, const std::string& group)
{
    using namespace neptune::core;
    if (track < 0 || track >= mixer::numTracks || ! trackActive (track) || trackType (track) != TrackType::Sampler) return 0;
    auto d = instrumentDescription (track);
    const int index = mapping::groupSwitch (d, group);
    if (index < 0) return 0;
    uiEditInstrument (track, std::move (d), "Group switch", {});   // sans effet si le Groupe avait déjà son interrupteur
    return mixer::modulatorParam (track, index);
}

void NeptuneProcessor::uiEditInstrument (int track, neptune::core::InstrumentDescription d, std::string name, std::string mergeKey)
{
    for (auto& z : d.zones) z = neptune::core::sanitised (z);
    auto previous = instrumentDescription (track);
    if (previous == d) return;
    // Commande « par différences » : un geste sur quelques zones d'un gros instrument ne copie pas toute la description.
    undo_.perform (std::make_unique<neptune::core::InstrumentEditCommand> (
        previous, d,
        [this, track] { return instrumentDescription (track); },
        [this, track] (const neptune::core::InstrumentDescription& v) { setInstrumentDescription (track, v, false); },
        std::move (name), std::move (mergeKey)));
}

void NeptuneProcessor::selectTrack (int track)
{
    track = juce::jlimit (0, neptune::core::mixer::numTracks - 1, track);
    if (track == selectedTrack_.load()) return;
    {
        const juce::ScopedLock lock (instrumentLock_);
        if (! mixer_.tracks[static_cast<std::size_t> (track)].active) return; // une piste inactive n'est pas sélectionnable
        mixer_.selectedTrack = track;
    }
    selectedTrack_.store (track);
    ++revision_;
    notifyStateChanged();
}

// ---- pistes : type, activité, patch du synthé
neptune::core::TrackType NeptuneProcessor::trackType (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))].type;
}

bool NeptuneProcessor::trackActive (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))].active;
}

int NeptuneProcessor::activeTrackCount() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.activeCount();
}

bool NeptuneProcessor::trackHasContent (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    const auto& t = mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))];
    return ! t.instrument.empty() || ! (t.synth == neptune::core::SynthDescription {}) || ! (t.fm == neptune::core::fm::FmDescription {})
        || ! (t.organ == neptune::core::organ::OrganDescription {});
}

neptune::core::SynthDescription NeptuneProcessor::synthDescription (int track) const
{
    const int t = juce::jlimit (0, neptune::core::mixer::numTracks - 1, track);
    const juce::ScopedLock lock (instrumentLock_);
    // temps d'enveloppe, glissando et équilibre : les valeurs courantes des paramètres (l'automation les a pu changer)
    return neptune::core::analogsynth::withLiveParams (mixer_.tracks[static_cast<std::size_t> (t)].synth, params_, t);
}

void NeptuneProcessor::uiEditSynth (neptune::core::SynthDescription d, std::string name, std::string mergeKey, bool presetChange)
{
    using namespace neptune::core;
    const int track = selectedTrack();
    d = sanitised (std::move (d));
    const auto previous = synthDescription (track);
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<SynthDescription>> (
        previous, d,
        [this, track, presetChange] (const SynthDescription& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.tracks[static_cast<std::size_t> (track)].synth = v; }
            publishSynthPatch (track, v, presetChange);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

neptune::core::fm::FmDescription NeptuneProcessor::fmDescription (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    const int t = juce::jlimit (0, neptune::core::mixer::numTracks - 1, track);
    return neptune::core::fm::withLiveParams (mixer_.tracks[static_cast<std::size_t> (t)].fm, params_, t);
}

void NeptuneProcessor::uiEditFm (neptune::core::fm::FmDescription d, std::string name, std::string mergeKey, bool presetChange)
{
    using namespace neptune::core;
    juce::ignoreUnused (presetChange); // une note FM garde le patch avec lequel elle a démarré : rien à figer
    const int track = selectedTrack();
    d = fm::sanitised (std::move (d));
    const auto previous = fmDescription (track);
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<fm::FmDescription>> (
        previous, d,
        [this, track] (const fm::FmDescription& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.tracks[static_cast<std::size_t> (track)].fm = v; }
            publishFmDescription (track, v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

neptune::core::organ::OrganDescription NeptuneProcessor::organDescription (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))].organ;
}

void NeptuneProcessor::uiEditOrgan (neptune::core::organ::OrganDescription d, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    const int track = selectedTrack();
    d = organ::sanitised (std::move (d));
    const auto previous = organDescription (track);
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<organ::OrganDescription>> (
        previous, d,
        [this, track] (const organ::OrganDescription& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.tracks[static_cast<std::size_t> (track)].organ = v; }
            publishOrganDescription (track, v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

void NeptuneProcessor::resetTrackParameters (int track, bool includeMixer)
{
    using namespace neptune::core;
    std::vector<ParamId> ids;
    for (int k = 0; k < mixer::kOrganKinds; ++k) ids.push_back (mixer::organParam (track, k));
    for (int k = 0; k < mixer::kFmKinds; ++k) ids.push_back (mixer::fmParam (track, static_cast<mixer::FmKind> (k)));
    for (int k = 0; k < mixer::kSamplerKinds; ++k) ids.push_back (mixer::samplerParam (track, static_cast<mixer::SamplerKind> (k)));
    for (int k = 0; k < mixer::kSynthKinds; ++k) ids.push_back (mixer::synthParam (track, static_cast<mixer::SynthKind> (k)));
    if (includeMixer)
    {
        for (const auto off : { probe::kVolume, probe::kPan, probe::kMute, probe::kSolo }) ids.push_back (probe::trackParam (track, off));
        for (int s = 0; s < mixer::numSends; ++s) ids.push_back (probe::trackParam (track, probe::kSendLevel + static_cast<ParamId> (s)));
    }
    for (const auto id : ids)
    {
        const auto index = params_.indexOf (id);
        if (index == ParameterSet::npos) continue;
        const auto& spec = params_.spec (index);
        if (params_.get (id) == spec.defaultValue) continue;
        uiResetToDefault (id);
    }
}

void NeptuneProcessor::uiSetTrackType (int track, neptune::core::TrackType type)
{
    using namespace neptune::core;
    track = juce::jlimit (0, mixer::numTracks - 1, track);
    {
        const juce::ScopedLock lock (instrumentLock_);
        auto& t = mixer_.tracks[static_cast<std::size_t> (track)];
        if (t.type == type) return;
        t.type = type;
        t.instrument = InstrumentDescription {};
        t.synth = SynthDescription {};
        t.fm = fm::FmDescription {};
        t.organ = organ::OrganDescription {};
        t.synthPresetId.clear();
    }
    engine_.setTrackType (track, type);
    publishSynthPatch (track, SynthDescription {});
    publishFmDescription (track, fm::FmDescription {});
    publishOrganDescription (track, organ::OrganDescription {});
    loader_->request (track, InstrumentDescription {}, false); // libère les samples de l'ancien instrument
    resetTrackParameters (track, false);
    undo_.clear(); // l'historique parlait de l'ancienne configuration de cette piste
    ++revision_;
    notifyStateChanged();
}

int NeptuneProcessor::uiAddTrack()
{
    using namespace neptune::core;
    int index = -1;
    {
        const juce::ScopedLock lock (instrumentLock_);
        index = mixer_.firstInactive();
        if (index < 0) return -1;
        mixer_.resetTrack (index);
        auto& t = mixer_.tracks[static_cast<std::size_t> (index)];
        t.active = true;
        t.type = TrackType::Synth;
    }
    auto clearTrackInserts = [this] (int track)
    {
        for (int s = 0; s < neptune::core::mixer::numInserts; ++s)
        {
            {
                const juce::ScopedLock lock (instrumentLock_);
                insertInstances_[static_cast<std::size_t> (track)][static_cast<std::size_t> (s)] = nullptr;
            }
            engine_.setInsert (StripKind::Track, track, s, nullptr);
        }
    };
    clearTrackInserts (index);
    resetTrackParameters (index, true);
    engine_.setTrackSettings (index, trackSettings (index));
    engine_.setTrackType (index, TrackType::Synth);
    engine_.setTrackActive (index, true);
    publishSynthPatch (index, SynthDescription {});
    publishFmDescription (index, fm::FmDescription {});
    publishOrganDescription (index, organ::OrganDescription {});
    loader_->request (index, InstrumentDescription {}, false);
    undo_.clear();
    selectTrack (index);
    ++revision_;
    notifyStateChanged();
    return index;
}

neptune::core::ConsoleMode NeptuneProcessor::consoleMode() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.console;
}

void NeptuneProcessor::uiSetConsoleMode (bool enabled, int track)
{
    {
        const juce::ScopedLock lock (instrumentLock_);
        mixer_.console.enabled = enabled;
        mixer_.console.track = juce::jlimit (-1, neptune::core::mixer::numTracks - 1, track);   // -1 : la Façade globale
    }
    notifyStateChanged();
}

neptune::core::facade::Facade NeptuneProcessor::globalFacade() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.globalFacade;
}

void NeptuneProcessor::uiEditGlobalFacade (neptune::core::facade::Facade f, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    f = facade::sanitised (std::move (f));
    const auto previous = globalFacade();
    if (previous == f) return;
    undo_.perform (std::make_unique<SnapshotCommand<facade::Facade>> (
        previous, f,
        [this] (const facade::Facade& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.globalFacade = v; }
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

neptune::core::router::Config NeptuneProcessor::routerConfig() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.router;
}

void NeptuneProcessor::uiEditRouter (neptune::core::router::Config c, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    c = router::sanitised (std::move (c));
    const auto previous = routerConfig();
    if (previous == c) return;
    undo_.perform (std::make_unique<SnapshotCommand<router::Config>> (
        previous, c,
        [this] (const router::Config& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.router = v; }
            engine_.setRouter (v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

neptune::core::combi::Library NeptuneProcessor::combinations() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.combinations;
}

void NeptuneProcessor::uiEditCombinations (neptune::core::combi::Library lib, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    lib = combi::sanitised (std::move (lib));
    const auto previous = combinations();
    if (previous == lib) return;
    undo_.perform (std::make_unique<SnapshotCommand<combi::Library>> (
        previous, lib,
        [this] (const combi::Library& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.combinations = v; }
            combi_.clampSequence (static_cast<int> (v.sequence.size()));   // la position du séquenceur suit une liste raccourcie (aussi à l'annulation)
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

NeptuneProcessor::CombiStatus NeptuneProcessor::combiStatus() const
{
    CombiStatus s;
    s.bank = combi_.bank();
    s.setArmed = combi_.armed();
    s.sequencePosition = combi_.sequencePosition();
    s.sequenceLength = static_cast<int> (combinations().sequence.size());
    return s;
}

neptune::core::combi::Result NeptuneProcessor::combiExecute (const std::function<neptune::core::combi::Outcome (neptune::core::combi::Library&)>& action)
{
    using namespace neptune::core;
    auto lib = combinations();
    // Les paramètres réglés par un rappel passent par la pile d'annulation et préviennent l'hôte ; le tout forme une seule entrée d'historique.
    combi_.setWriter ([this] (ParamId id, double real)
    {
        const auto index = params_.indexOf (id);
        if (index == ParameterSet::npos) return;
        auto* hp = hostParam (index);
        if (hp != nullptr) hp->beginChangeGesture();
        undo_.setParameter (params_, id, real);
        if (hp != nullptr)
        {
            hp->setValueNotifyingHost (static_cast<float> (params_.getNormalizedAt (index)));
            hp->endChangeGesture();
        }
    });
    undo_.beginTransaction ("Combination");
    const auto outcome = action (lib);
    if (outcome.result == combi::Result::Stored) uiEditCombinations (std::move (lib), "Combination: store");
    undo_.endTransaction();
    return outcome.result;
}

neptune::core::combi::Result NeptuneProcessor::uiCombiCommand (int command, int track)
{
    using namespace neptune::core;
    const int t = juce::jlimit (0, mixer::numTracks - 1, track);
    return combiExecute ([this, command, t] (combi::Library& lib) { return combi_.run (command, lib, params_, t); });
}

neptune::core::combi::Result NeptuneProcessor::uiCombiProgramChange (int program, int track)
{
    return combiExecute ([this, program, track] (neptune::core::combi::Library& lib) { return combi_.programChange (lib, params_, program, track); });
}

int NeptuneProcessor::trackForMidiChannel (int channel) const
{
    using namespace neptune::core;
    int best = -1, bestCount = 99;
    for (int t = 0; t < mixer::numTracks; ++t)
    {
        if (! trackActive (t)) continue;
        const unsigned mask = mixer::midiChannelsOf (trackSettings (t));
        if ((mask & (1u << channel)) == 0) continue;
        int count = 0;
        for (unsigned m = mask; m != 0; m &= m - 1) ++count;
        if (count < bestCount) { best = t; bestCount = count; }
    }
    return best;
}

void NeptuneProcessor::drainCombinationPrograms()
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    combiProgramFifo_.prepareToRead (combiProgramFifo_.getNumReady(), start1, size1, start2, size2);
    std::vector<int> items;
    for (int i = 0; i < size1; ++i) items.push_back (combiPrograms_[static_cast<std::size_t> (start1 + i)].load (std::memory_order_relaxed));
    for (int i = 0; i < size2; ++i) items.push_back (combiPrograms_[static_cast<std::size_t> (start2 + i)].load (std::memory_order_relaxed));
    combiProgramFifo_.finishedRead (size1 + size2);
    for (const int item : items)
    {
        const int channel = (item >> 8) & 15, program = item & 127;
        uiCombiProgramChange (program, trackForMidiChannel (channel));
    }
}

bool NeptuneProcessor::combiLit (int command, int track) const
{
    using namespace neptune::core;
    if (command < combi::kCommandGeneral || command >= combi::kCommandDivisional + combi::kPistons) return false;
    const auto lib = combinations();
    const auto* memory = command < combi::kCommandDivisional ? lib.general (combi_.bank(), command - combi::kCommandGeneral)
                                                              : lib.divisional (track, combi_.bank(), command - combi::kCommandDivisional);
    return memory != nullptr && combi::matches (params_, *memory);
}

neptune::core::trackexport::Bundle NeptuneProcessor::exportTrackBundle (int track) const
{
    using namespace neptune::core;
    track = juce::jlimit (0, mixer::numTracks - 1, track);
    trackexport::Bundle b;
    b.track = track;
    b.state = mixerState().tracks[static_cast<std::size_t> (track)];
    for (const auto id : trackexport::ownedParams (params_, track))
    {
        const auto index = params_.indexOf (id);
        if (index != ParameterSet::npos && params_.getAt (index) != params_.spec (index).defaultValue) b.params[id] = params_.getAt (index);
    }
    return b;
}

int NeptuneProcessor::uiImportTrack (neptune::core::trackexport::Bundle b)
{
    using namespace neptune::core;
    namespace mx = neptune::core::mixer;
    const int index = uiAddTrack();   // piste neuve, sélectionnée, tous ses réglages à leur défaut
    if (index < 0) return -1;
    trackexport::remapTo (b, index);
    auto track = b.state;
    {
        const juce::ScopedLock lock (instrumentLock_);
        auto& fresh = mixer_.tracks[static_cast<std::size_t> (index)];
        track.strip.midiChannel = fresh.strip.midiChannel;
        track.strip.midiChannelMask = fresh.strip.midiChannelMask;
        track.strip = mx::sanitisedStrip (std::move (track.strip), false, index);
        track.active = true;
        fresh = track;
    }
    engine_.setTrackType (index, track.type);
    engine_.setTrackSettings (index, track.strip);
    publishSynthPatch (index, track.synth);
    publishFmDescription (index, track.fm);
    publishOrganDescription (index, track.organ);
    for (int s = 0; s < mx::numInserts; ++s)
    {
        std::shared_ptr<InsertInstance> inst;
        const auto& st = track.strip.inserts[static_cast<std::size_t> (s)];
        if (! st.empty())
        {
            inst = createInsert (st.type, engine_.sampleRate(), engine_.maxBlockSize());
            if (inst != nullptr)
            {
                for (const auto& [id, v] : st.values) inst->runner.params().set (id, v);
                if (! st.text.empty() && inst->runner.module().hasStateString()) inst->runner.module().setStateString (st.text);
            }
        }
        {
            const juce::ScopedLock lock (instrumentLock_);
            insertInstances_[static_cast<std::size_t> (index)][static_cast<std::size_t> (s)] = inst;
        }
        engine_.setInsert (StripKind::Track, index, s, inst);
    }
    loader_->request (index, track.instrument, false);
    std::vector<std::pair<ParamId, double>> values (b.params.begin(), b.params.end());
    seedHostParameters (values);
    undo_.clear();
    ++revision_;
    notifyStateChanged();
    return index;
}

void NeptuneProcessor::installSamplerTrack (int index, neptune::core::InstrumentDescription instrument, const std::string& name, int midiChannel)
{
    using namespace neptune::core;
    namespace mx = neptune::core::mixer;
    {
        const juce::ScopedLock lock (instrumentLock_);
        mixer_.resetTrack (index);
        auto& t = mixer_.tracks[static_cast<std::size_t> (index)];
        t.active = true;
        t.type = TrackType::Sampler;
        t.instrument = std::move (instrument);
        t.strip.name = name;
        if (midiChannel >= 1)
        {
            t.strip.midiChannel = midiChannel;
            t.strip.midiChannelMask = 0;
        }
        t.strip = mx::sanitisedStrip (std::move (t.strip), false, index);
    }
    for (int s = 0; s < mx::numInserts; ++s)
    {
        { const juce::ScopedLock lock (instrumentLock_); insertInstances_[static_cast<std::size_t> (index)][static_cast<std::size_t> (s)] = nullptr; }
        engine_.setInsert (StripKind::Track, index, s, nullptr);
    }
    resetTrackParameters (index, true);
    engine_.setTrackSettings (index, trackSettings (index));
    engine_.setTrackType (index, TrackType::Sampler);
    engine_.setTrackActive (index, true);
    publishSynthPatch (index, SynthDescription {});
    publishFmDescription (index, fm::FmDescription {});
    publishOrganDescription (index, organ::OrganDescription {});
    InstrumentDescription description;
    { const juce::ScopedLock lock (instrumentLock_); description = mixer_.tracks[static_cast<std::size_t> (index)].instrument; }
    loader_->request (index, description, false);
}

// Place un instrument importé (SFZ, SoundFont) : soit il remplace tout le mixeur (une seule piste Sampler), soit il va sur une piste libre. Faux : plus de piste libre.
bool NeptuneProcessor::installImportedInstrument (const neptune::core::InstrumentDescription& instrument, const std::string& name, SfzPlacement placement)
{
    using namespace neptune::core;
    if (placement == SfzPlacement::ClearMixer)
    {
        // toutes les pistes sauf la première active disparaissent ; celle-ci est remise à zéro et reçoit l'instrument
        int keep = -1;
        std::vector<int> others;
        {
            const juce::ScopedLock lock (instrumentLock_);
            for (int t = 0; t < mixer::numTracks; ++t)
                if (mixer_.tracks[static_cast<std::size_t> (t)].active) { if (keep < 0) keep = t; else others.push_back (t); }
        }
        for (const int t : others) uiRemoveTrack (t);
        const int target = keep < 0 ? 0 : keep;
        installSamplerTrack (target, instrument, name, -1);
        selectTrack (target);
    }
    else
    {
        int target = -1;
        {
            const juce::ScopedLock lock (instrumentLock_);
            if (mixer_.tracks[0].active && ! trackHasContent (0)) target = 0;   // la piste de départ, restée vide : l'instrument la remplace
            else target = mixer_.firstInactive();
        }
        if (target < 0) return false;
        installSamplerTrack (target, instrument, name, -1);
        selectTrack (target);
    }
    undo_.clear();
    ++revision_;
    notifyStateChanged();
    return true;
}

neptune::core::sfzimport::Result NeptuneProcessor::uiImportSfz (const std::filesystem::path& sfzFile, SfzPlacement placement)
{
    using namespace neptune::core;
    auto result = sfzimport::importSfzFile (sfzFile);
    if (! result.ok) return result;
    if (! installImportedInstrument (result.instrument, result.name, placement))
    {
        result.ok = false;
        result.error = "no_free_track";
    }
    return result;
}

neptune::core::soundfont::Result NeptuneProcessor::uiImportSoundFont (const std::filesystem::path& bankFile, int presetIndex, SfzPlacement placement)
{
    using namespace neptune::core;
    auto result = soundfont::importPreset (bankFile, presetIndex);
    if (! result.ok) return result;
    if (! installImportedInstrument (result.instrument, result.name, placement))
    {
        result.ok = false;
        result.error = "no_free_track";
    }
    return result;
}

NeptuneProcessor::OrganMemory NeptuneProcessor::uiEstimateOrganMemory (const std::filesystem::path& organFile, const std::vector<std::string>& excludedPerspectives)
{
    using namespace neptune::core;
    organimport::Options probe;
    probe.baseDir = organFile.parent_path();
    probe.readSampleMarks = false;
    probe.excludedPerspectives = excludedPerspectives;
    auto counted = organimport::importOrganFile (organFile, probe);
    OrganMemory m;
    if (! counted.ok) return m;
    memory::FileInfoCache cache;
    for (auto& kb : counted.keyboards)
    {
        kb.instrument.streamFromDisk = false;
        m.needed += memory::estimateInstrumentBytes (kb.instrument, cache);
    }
    m.headroom = memory::availablePhysicalBytes();
    return m;
}

neptune::core::organimport::Result NeptuneProcessor::uiImportOrgan (const std::filesystem::path& organFile, bool streamFromDisk, const std::vector<std::string>& excludedPerspectives)
{
    using namespace neptune::core;
    namespace mx = neptune::core::mixer;
    int first = -1;
    int wanted = 0;
    {
        // le nombre de claviers n'est connu qu'après lecture : une première lecture sans fichiers d'échantillons sert à le compter
        organimport::Options probe;
        probe.baseDir = organFile.parent_path();
        probe.readSampleMarks = false;
        const auto counted = organimport::importOrganFile (organFile, probe);
        if (! counted.ok) return counted;
        wanted = static_cast<int> (counted.keyboards.size());
        const juce::ScopedLock lock (instrumentLock_);
        for (int start = 0; start + wanted <= mx::numTracks && first < 0; ++start)
        {
            bool free = true;
            for (int k = 0; k < wanted && free; ++k)
            {
                const auto& candidate = mixer_.tracks[static_cast<std::size_t> (start + k)];
                const bool pristine = start + k == 0 && candidate.active && ! trackHasContent (0);   // la piste de départ, restée vide : l'orgue la remplace
                free = ! candidate.active || pristine;
            }
            if (free) first = start;
        }
    }
    if (first < 0)
    {
        organimport::Result r;
        r.error = "Not enough free tracks (" + std::to_string (wanted) + " needed)";
        return r;
    }
    organimport::Options opt;
    opt.baseDir = organFile.parent_path();
    opt.firstTrack = first;
    opt.firstChannel = 1;
    opt.stylePack = "organ-wood-brass";   // style par défaut des pages générées (bois et laiton)
    opt.streamFromDisk = streamFromDisk;
    opt.excludedPerspectives = excludedPerspectives;
    auto result = organimport::importOrganFile (organFile, opt);
    if (! result.ok) return result;

    for (std::size_t k = 0; k < result.keyboards.size(); ++k)
    {
        const int index = first + static_cast<int> (k);
        const auto& kb = result.keyboards[k];
        installSamplerTrack (index, kb.instrument, kb.name, kb.midiChannel);
    }
    {
        const juce::ScopedLock lock (instrumentLock_);
        for (auto& rule : result.router.rules) mixer_.router.rules.push_back (rule);
        mixer_.router = router::sanitised (std::move (mixer_.router));
        for (auto& [key, memory] : result.combinations.generals) mixer_.combinations.generals[key] = memory;
        for (auto& [key, memory] : result.combinations.divisionals) mixer_.combinations.divisionals[key] = memory;
        if (mixer_.combinations.sequence.empty()) mixer_.combinations.sequence = result.combinations.sequence;
        mixer_.combinations = combi::sanitised (std::move (mixer_.combinations));
        engine_.setRouter (mixer_.router);
        // la Façade de l'orgue : remplace une Façade globale vide, sinon s'ajoute à la suite de ses pages
        if (mixer_.globalFacade.widgets.empty() && mixer_.globalFacade.pages.empty()) mixer_.globalFacade = result.facade;
        else
        {
            auto first = result.facade;
            first.pages.clear();
            mixer_.globalFacade.pages.push_back (std::move (first));
            for (const auto& page : result.facade.pages) mixer_.globalFacade.pages.push_back (page);
        }
        mixer_.globalFacade = facade::sanitised (std::move (mixer_.globalFacade));
    }
    seedHostParameters (result.initialValues);
    selectTrack (first);
    undo_.clear();
    ++revision_;
    notifyStateChanged();
    return result;
}

bool NeptuneProcessor::uiRemoveTrack (int track)
{
    using namespace neptune::core;
    track = juce::jlimit (0, mixer::numTracks - 1, track);
    int selected = 0;
    std::array<bool, mixer::numParallels> wasActive {};
    std::vector<int> goneParallels;
    {
        const juce::ScopedLock lock (instrumentLock_);
        if (! mixer_.tracks[static_cast<std::size_t> (track)].active || mixer_.activeCount() <= 1) return false;
        for (int p = 0; p < mixer::numParallels; ++p) // les pistes parallèles qui disparaissent avec la piste
            if (mixer_.parallels[static_cast<std::size_t> (p)].active) wasActive[static_cast<std::size_t> (p)] = true;
        mixer_.resetTrack (track); // remet aussi la sélection sur une piste active si c'était celle-ci
        selected = mixer_.selectedTrack;
        for (int p = 0; p < mixer::numParallels; ++p)
            if (wasActive[static_cast<std::size_t> (p)] && ! mixer_.parallels[static_cast<std::size_t> (p)].active) goneParallels.push_back (p);
    }
    for (const int p : goneParallels) dropParallel (p);
    auto clearTrackInserts = [this] (int track)
    {
        for (int s = 0; s < neptune::core::mixer::numInserts; ++s)
        {
            {
                const juce::ScopedLock lock (instrumentLock_);
                insertInstances_[static_cast<std::size_t> (track)][static_cast<std::size_t> (s)] = nullptr;
            }
            engine_.setInsert (StripKind::Track, track, s, nullptr);
        }
    };
    clearTrackInserts (track);
    engine_.setTrackActive (track, false);
    engine_.setTrackType (track, TrackType::Sampler);
    engine_.setTrackSettings (track, trackSettings (track));
    publishSynthPatch (track, SynthDescription {});
    publishFmDescription (track, fm::FmDescription {});
    publishOrganDescription (track, organ::OrganDescription {});
    loader_->request (track, InstrumentDescription {}, false);
    resetTrackParameters (track, true);
    undo_.clear();
    selectedTrack_.store (selected);
    ++revision_;
    notifyStateChanged();
    return true;
}

// ---- pistes parallèles
bool NeptuneProcessor::parallelActive (int p) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.parallels[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numParallels - 1, p))].active;
}

neptune::core::mixer::ParallelSettings NeptuneProcessor::parallelSettings (int p) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.parallels[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numParallels - 1, p))];
}

int NeptuneProcessor::parallelDepth (int p) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.parallelDepth (p);
}

std::vector<int> NeptuneProcessor::parallelChildren (int parentId) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.parallelChildren (parentId);
}

// Arrête une piste parallèle déjà retirée de l'état : plus de traitement, inserts libérés, paramètres remis à leur valeur d'origine.
void NeptuneProcessor::dropParallel (int p)
{
    namespace mx = neptune::core::mixer;
    engine_.setParallel (p, false, 0, mx::TapPoint::PostPan);
    for (int s = 0; s < mx::numInserts; ++s)
    {
        {
            const juce::ScopedLock lock (instrumentLock_);
            insertInstances_[static_cast<std::size_t> (insertFlatIndex (StripKind::Parallel, p))][static_cast<std::size_t> (s)] = nullptr;
        }
        engine_.setInsert (StripKind::Parallel, p, s, nullptr);
    }
    std::vector<neptune::core::ParamId> ids { mx::parallelParam (p, mx::kParVolume), mx::parallelParam (p, mx::kParPan),
                                              mx::parallelParam (p, mx::kParMute), mx::parallelParam (p, mx::kParSolo) };
    for (int s = 0; s < mx::numSends; ++s) ids.push_back (mx::parallelParam (p, mx::kParSendLevel + static_cast<neptune::core::ParamId> (s)));
    for (int s = 0; s < mx::numInserts; ++s) ids.push_back (mx::parallelParam (p, mx::kParInsertBypass + static_cast<neptune::core::ParamId> (s)));
    for (const auto id : ids)
    {
        const auto index = params_.indexOf (id);
        if (index == neptune::core::ParameterSet::npos) continue;
        if (params_.get (id) == params_.spec (index).defaultValue) continue;
        uiResetToDefault (id);
    }
}

int NeptuneProcessor::uiAddParallel (int parent)
{
    namespace mx = neptune::core::mixer;
    int p = -1;
    {
        const juce::ScopedLock lock (instrumentLock_);
        if (parent < 0 || parent >= mx::numTracks + mx::numParallels) return -1;
        if (parent < mx::numTracks ? ! mixer_.tracks[static_cast<std::size_t> (parent)].active
                                   : ! mixer_.parallels[static_cast<std::size_t> (parent - mx::numTracks)].active)
            return -1;
        p = mixer_.firstInactiveParallel();
        if (p < 0) return -1;
        mx::ParallelSettings ps;
        ps.active = true;
        ps.parent = parent;
        mixer_.parallels[static_cast<std::size_t> (p)] = ps;
    }
    dropParallel (p);      // valeurs d'origine (au cas où d'anciennes valeurs d'automation traînent), puis activation
    engine_.setParallelSettings (p, parallelSettings (p).strip);
    engine_.setParallel (p, true, parent, mx::TapPoint::PostPan);
    undo_.clear();
    ++revision_;
    notifyStateChanged();
    return p;
}

int NeptuneProcessor::uiRemoveParallel (int p)
{
    namespace mx = neptune::core::mixer;
    std::vector<int> removed;
    {
        const juce::ScopedLock lock (instrumentLock_);
        removed = mixer_.removeParallel (p);
    }
    for (const int r : removed) dropParallel (r);
    if (! removed.empty())
    {
        undo_.clear();
        ++revision_;
        notifyStateChanged();
    }
    return static_cast<int> (removed.size());
}

void NeptuneProcessor::uiEditParallel (int p, neptune::core::mixer::ParallelSettings s, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    p = juce::jlimit (0, mixer::numParallels - 1, p);
    s = mixer::sanitised (std::move (s), p);
    const auto previous = parallelSettings (p);
    if (! previous.active || previous == s) return;
    s.active = true;
    undo_.perform (std::make_unique<SnapshotCommand<mixer::ParallelSettings>> (
        previous, s,
        [this, p] (const mixer::ParallelSettings& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.parallels[static_cast<std::size_t> (p)] = v; }
            engine_.setParallelSettings (p, v.strip);
            engine_.setParallel (p, v.active, v.parent, v.tap, v.followParentVolume);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

// ---- réglages du mixeur
namespace
{
// Recopie le type et les valeurs des modules vivants dans l'état à sauvegarder.
void syncInserts (std::array<neptune::core::mixer::InsertState, neptune::core::mixer::numInserts>& out,
                  const std::array<std::shared_ptr<neptune::core::InsertInstance>, neptune::core::mixer::numInserts>& live)
{
    for (std::size_t s = 0; s < out.size(); ++s)
    {
        out[s] = {};
        if (live[s] == nullptr) continue;
        out[s].type = live[s]->type;
        const auto& ps = live[s]->runner.params();
        for (std::size_t i = 0; i < ps.size(); ++i) out[s].values[ps.spec (i).id] = ps.getAt (i);
        if (live[s]->runner.module().hasStateString()) out[s].text = live[s]->runner.module().stateString();
    }
}
} // namespace

neptune::core::MixerState NeptuneProcessor::mixerState() const
{
    const juce::ScopedLock lock (instrumentLock_);
    auto m = mixer_;
    namespace mx = neptune::core::mixer;
    for (int t = 0; t < mx::numTracks; ++t)
    {
        auto& tr = m.tracks[static_cast<std::size_t> (t)];
        syncInserts (tr.strip.inserts, insertInstances_[static_cast<std::size_t> (t)]);
        tr.synth = neptune::core::analogsynth::withLiveParams (tr.synth, params_, t);
        tr.fm = neptune::core::fm::withLiveParams (tr.fm, params_, t);
    }
    for (int b = 0; b < mx::numBuses; ++b) syncInserts (m.buses[static_cast<std::size_t> (b)].inserts, insertInstances_[static_cast<std::size_t> (mx::numTracks + b)]);
    syncInserts (m.master.inserts, insertInstances_[static_cast<std::size_t> (mx::numTracks + mx::numBuses)]);
    for (int p = 0; p < mx::numParallels; ++p)
        syncInserts (m.parallels[static_cast<std::size_t> (p)].strip.inserts, insertInstances_[static_cast<std::size_t> (mx::numTracks + mx::numBuses + 1 + p)]);
    return m;
}

// ---- inserts
int NeptuneProcessor::insertFlatIndex (StripKind kind, int index) noexcept
{
    namespace mx = neptune::core::mixer;
    switch (kind)
    {
        case StripKind::Track: return juce::jlimit (0, mx::numTracks - 1, index);
        case StripKind::Bus: return mx::numTracks + juce::jlimit (0, mx::numBuses - 1, index);
        case StripKind::Parallel: return mx::numTracks + mx::numBuses + 1 + juce::jlimit (0, mx::numParallels - 1, index);
        default: return mx::numTracks + mx::numBuses;
    }
}

std::string NeptuneProcessor::insertType (StripKind kind, int index, int slot) const
{
    const juce::ScopedLock lock (instrumentLock_);
    const auto& inst = insertInstances_[static_cast<std::size_t> (insertFlatIndex (kind, index))]
                                       [static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numInserts - 1, slot))];
    return inst != nullptr ? inst->type : std::string();
}

neptune::core::ParamId NeptuneProcessor::insertBypassParam (StripKind kind, int index, int slot) const
{
    namespace mx = neptune::core::mixer;
    slot = juce::jlimit (0, mx::numInserts - 1, slot);
    switch (kind)
    {
        case StripKind::Track: return neptune::core::probe::trackParam (index, neptune::core::probe::kInsertBypass + static_cast<neptune::core::ParamId> (slot));
        case StripKind::Bus: return mx::busParam (index, mx::kBusInsertBypass + static_cast<neptune::core::ParamId> (slot));
        case StripKind::Parallel: return mx::parallelParam (index, mx::kParInsertBypass + static_cast<neptune::core::ParamId> (slot));
        default: return mx::masterInsertBypassId (slot);
    }
}

bool NeptuneProcessor::insertBypassed (StripKind kind, int index, int slot) const
{
    return params_.get (insertBypassParam (kind, index, slot)) >= 0.5;
}

void NeptuneProcessor::uiSetInsert (StripKind kind, int index, int slot, const std::string& type)
{
    namespace mx = neptune::core::mixer;
    slot = juce::jlimit (0, mx::numInserts - 1, slot);
    std::shared_ptr<neptune::core::InsertInstance> inst;
    if (! type.empty())
    {
        inst = neptune::core::createInsert (type, engine_.sampleRate(), engine_.maxBlockSize());
        if (inst == nullptr) return;
        // Fournaise dans Neptune : sans suréchantillonnage, donc sans latence (celle-ci n'est pas compensée entre les pistes) ; le plugin séparé garde 2x
        if (type == "overdrive") inst->runner.params().set (neptune::core::overdrive::oversampling, 0.0);
    }
    {
        const juce::ScopedLock lock (instrumentLock_);
        insertInstances_[static_cast<std::size_t> (insertFlatIndex (kind, index))][static_cast<std::size_t> (slot)] = inst;
    }
    engine_.setInsert (kind, index, slot, inst);
    uiResetToDefault (insertBypassParam (kind, index, slot)); // un nouvel insert n'est pas contourné
    undo_.clear(); // l'historique pouvait pointer vers les paramètres de l'ancien module
    ++revision_;
    notifyStateChanged();
}

// Adaptateur : les gestes d'édition d'un module passent par l'historique d'annulation du plugin.
class NeptuneProcessor::InsertHost : public ParamHost
{
public:
    InsertHost (NeptuneProcessor& owner, std::shared_ptr<neptune::core::InsertInstance> inst) : owner_ (owner), inst_ (std::move (inst)) {}

    neptune::core::ParameterSet& parameters() override { return inst_->runner.params(); }
    void uiBeginEdit (neptune::core::ParamId) override { owner_.undo_.beginGesture(); }
    void uiSetNormalised (neptune::core::ParamId id, double n) override
    {
        auto& ps = parameters();
        const auto index = ps.indexOf (id);
        if (index == neptune::core::ParameterSet::npos) return;
        owner_.undo_.setParameter (ps, id, ps.spec (index).fromNormalized (n));
        ++owner_.revision_;
        owner_.notifyStateChanged();
    }
    void uiEndEdit (neptune::core::ParamId) override { owner_.undo_.endGesture(); }
    void uiResetToDefault (neptune::core::ParamId id) override
    {
        auto& ps = parameters();
        const auto index = ps.indexOf (id);
        if (index == neptune::core::ParameterSet::npos) return;
        uiBeginEdit (id);
        uiSetNormalised (id, ps.spec (index).toNormalized (ps.spec (index).defaultValue));
        uiEndEdit (id);
    }
    void uiEditText (std::function<void (const std::string&)> apply, std::string oldValue, std::string newValue, std::string name) override
    {
        if (oldValue == newValue) return;
        owner_.undo_.perform (std::make_unique<neptune::core::SnapshotCommand<std::string>> (std::move (oldValue), std::move (newValue), std::move (apply), std::move (name)));
        ++owner_.revision_;
        owner_.notifyStateChanged();
    }
    bool uiUndo() override { return owner_.uiUndo(); }
    bool uiRedo() override { return owner_.uiRedo(); }
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return owner_.modalSize (t); }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { owner_.setModalSize (t, s); }

private:
    NeptuneProcessor& owner_;
    std::shared_ptr<neptune::core::InsertInstance> inst_;
};

NeptuneProcessor::InsertHandle NeptuneProcessor::openInsert (StripKind kind, int index, int slot)
{
    InsertHandle h;
    {
        const juce::ScopedLock lock (instrumentLock_);
        h.instance = insertInstances_[static_cast<std::size_t> (insertFlatIndex (kind, index))]
                                     [static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numInserts - 1, slot))];
    }
    if (h.instance != nullptr) h.host = std::make_unique<InsertHost> (*this, h.instance);
    return h;
}

std::vector<std::pair<neptune::core::ParamId, std::string>> NeptuneProcessor::insertModulationTargets (int track) const
{
    namespace mod = neptune::core::modulation;
    std::vector<std::pair<neptune::core::ParamId, std::string>> out;
    for (int slot = 0; slot < neptune::core::mixer::numInserts; ++slot)
    {
        std::shared_ptr<neptune::core::InsertInstance> inst;
        {
            const juce::ScopedLock lock (instrumentLock_);
            inst = insertInstances_[static_cast<std::size_t> (insertFlatIndex (StripKind::Track, track))][static_cast<std::size_t> (slot)];
        }
        if (inst == nullptr) continue;
        const auto& ps = inst->runner.params();
        for (std::size_t i = 0; i < ps.size(); ++i)
        {
            const auto& spec = ps.spec (i);
            if (spec.hidden || ! spec.modulatable) continue;
            out.emplace_back (mod::insertDest (slot, spec.id),
                              "Track " + std::to_string (track + 1) + " Insert " + std::to_string (slot + 1) + " (" + inst->type + ") " + spec.name);
        }
    }
    return out;
}

std::string NeptuneProcessor::modulationDestName (int track, neptune::core::ParamId dest) const
{
    namespace mod = neptune::core::modulation;
    if (! mod::isInsertDest (dest))
    {
        const auto* spec = params_.find (dest);
        return spec != nullptr ? spec->name : std::string();
    }
    for (const auto& [id, name] : insertModulationTargets (track))
        if (id == dest) return name;
    return {};
}

std::pair<float, float> NeptuneProcessor::compressorReduction (StripKind kind, int index) const
{
    namespace cm = neptune::core::compressor;
    float left = 0.0f, right = 0.0f;
    for (int slot = 0; slot < neptune::core::mixer::numInserts; ++slot)
    {
        std::shared_ptr<neptune::core::InsertInstance> inst;
        {
            const juce::ScopedLock lock (instrumentLock_);
            inst = insertInstances_[static_cast<std::size_t> (insertFlatIndex (kind, index))][static_cast<std::size_t> (slot)];
        }
        if (inst == nullptr || inst->type != "compressor" || insertBypassed (kind, index, slot)) continue;
        const auto& module = inst->runner.module();
        if (module.numMeters() <= cm::kReductionR) continue;
        left = std::max (left, -module.meter (cm::kReductionL));
        right = std::max (right, -module.meter (cm::kReductionR));
    }
    return { std::max (0.0f, left), std::max (0.0f, right) };
}

neptune::core::mixer::StripSettings NeptuneProcessor::trackSettings (int track) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numTracks - 1, track))].strip;
}

neptune::core::mixer::StripSettings NeptuneProcessor::busSettings (int bus) const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.buses[static_cast<std::size_t> (juce::jlimit (0, neptune::core::mixer::numBuses - 1, bus))];
}

neptune::core::mixer::MasterSettings NeptuneProcessor::masterSettings() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.master;
}

void NeptuneProcessor::publishFmDescription (int track, const neptune::core::fm::FmDescription& d)
{
    engine_.publishFm (track, std::make_shared<const neptune::core::fm::FmDescription> (neptune::core::fm::sanitised (d)));
    if (! neptune::core::fm::extraParamsMatch (d, params_, track)) // LFO, bend, molette, glissando suivent la description
        seedHostParameters (neptune::core::fm::extraParamValues (d, track));
}

void NeptuneProcessor::publishOrganDescription (int track, const neptune::core::organ::OrganDescription& d)
{
    engine_.publishOrgan (track, std::make_shared<const neptune::core::organ::OrganDescription> (neptune::core::organ::sanitised (d)));
}

void NeptuneProcessor::publishSynthPatch (int track, const neptune::core::SynthDescription& d, bool freezeRunningVoices)
{
    auto patch = std::make_shared<neptune::core::SynthPatch> (d);
    patch->freezeRunningVoices = freezeRunningVoices;
    engine_.publishSynth (track, std::move (patch));
    // Les paramètres d'enveloppe, de glissando et d'équilibre suivent la description (après la publication : les notes figées par
    // la capture par voix gardent les valeurs d'avant). L'hôte est prévenu pour que ses boutons et son automation suivent.
    if (! neptune::core::analogsynth::envelopeParamsMatch (d, params_, track))
        seedHostParameters (neptune::core::analogsynth::envelopeParamValues (d, track));
}

void NeptuneProcessor::seedHostParameters (const std::vector<std::pair<neptune::core::ParamId, double>>& values)
{
    for (const auto& [pid, raw] : values)
    {
        const auto index = params_.indexOf (pid);
        if (index == neptune::core::ParameterSet::npos) continue;
        const double v = params_.spec (index).clamp (raw);
        if (params_.getAt (index) == v) continue;
        params_.set (pid, v);
        if (! notifyHostOnSeed_) continue; // état en cours de chargement : l'hôte relit tout après
        if (auto* hp = hostParam (index))
        {
            hp->beginChangeGesture();
            hp->setValueNotifyingHost (static_cast<float> (params_.getNormalizedAt (index)));
            hp->endChangeGesture();
        }
    }
}

// ---- presets de synthé de la piste sélectionnée
void NeptuneProcessor::setTrackPresetId (int track, const std::string& id)
{
    {
        const juce::ScopedLock lock (instrumentLock_);
        mixer_.tracks[static_cast<std::size_t> (track)].synthPresetId = id;
    }
    ++presetRevision_;
    notifyStateChanged();
}

std::string NeptuneProcessor::currentPresetId() const
{
    const juce::ScopedLock lock (instrumentLock_);
    return mixer_.tracks[static_cast<std::size_t> (selectedTrack())].synthPresetId;
}

const neptune::core::preset::Preset* NeptuneProcessor::currentPreset() const
{
    const auto id = currentPresetId();
    return id.empty() ? nullptr : presetLibrary_->find (id);
}

bool NeptuneProcessor::acceptsPreset (const neptune::core::preset::Preset& p) const
{
    using namespace neptune::core;
    if (p.module.empty()) return true;
    const auto t = trackType (selectedTrack());
    if (t == TrackType::Organ) return p.module == organ::moduleId;
    if (t == TrackType::Fm) return p.module == fm::moduleId;
    if (t == TrackType::Synth) return p.module == analogsynth::moduleId;
    return false;
}

std::vector<std::string> NeptuneProcessor::presetKeys() const
{
    using namespace neptune::core;
    std::vector<std::string> keys;
    if (trackType (selectedTrack()) == TrackType::Organ)
        for (const auto& k : organ::keyedParams()) keys.push_back (k.key);
    else if (trackType (selectedTrack()) == TrackType::Fm)
        for (const auto& k : fm::keyedParams()) keys.push_back (k.key);
    else
    {
        for (const auto& k : analogsynth::keyedParams()) keys.push_back (k.key);
        keys.push_back (analogsynth::glideKey);
    }
    return keys;
}

bool NeptuneProcessor::loadPreset (const std::string& id)
{
    using namespace neptune::core;
    const int track = selectedTrack();
    const auto type = trackType (track);
    if (type != TrackType::Synth && type != TrackType::Fm && type != TrackType::Organ) return false;
    const auto* p = presetLibrary_->find (id);
    if (p == nullptr || ! acceptsPreset (*p)) return false;
    const auto& cfg = presetLibrary_->config();

    undo_.beginTransaction ("Preset");
    std::vector<std::pair<ParamId, double>> values; // identifiants de la piste, valeurs à écrire
    if (type == TrackType::Organ)
    {
        // l'orgue n'a que des valeurs (les claviers ne font pas partie d'un preset) ; une clé absente remet le réglage à son défaut
        for (const auto& k : organ::keyedParams())
        {
            if (preset::captureOf (k.key, *p, cfg) == preset::Capture::Free) continue;
            const ParamId pid = organ::idForTrack (k.id, track);
            const auto index = params_.indexOf (pid);
            if (index == ParameterSet::npos) continue;
            const auto it = p->parameters.find (k.key);
            values.emplace_back (pid, it != p->parameters.end() && ! it->second.isText ? it->second.number : params_.spec (index).defaultValue);
        }
    }
    else if (type == TrackType::Fm)
    {
        uiEditFm (fm::presetDescription (*p, fmDescription (track), cfg), "Preset", {}, true);
        // macros : celles du preset, sinon leur défaut (un patch importé ne porte pas de macros)
        for (const auto& k : fm::keyedParams())
        {
            if (preset::captureOf (k.key, *p, cfg) == preset::Capture::Free) continue;
            const ParamId pid = fm::idForTrack (k.id, track);
            const auto index = params_.indexOf (pid);
            if (index == ParameterSet::npos) continue;
            const auto it = p->parameters.find (k.key);
            values.emplace_back (pid, it != p->parameters.end() && ! it->second.isText ? it->second.number : params_.spec (index).defaultValue);
        }
    }
    else
    {
        // le patch d'abord (capture par voix : l'audio fige les notes en cours avec les valeurs d'avant), puis les valeurs
        uiEditSynth (analogsynth::presetDescription (*p, synthDescription (track), cfg), "Preset", {}, true);
        for (const auto& [key, value] : p->parameters)
        {
            const ParamId base = analogsynth::paramIdForKey (key);
            if (base == 0 || value.isText || preset::captureOf (key, *p, cfg) == preset::Capture::Free) continue;
            values.emplace_back (analogsynth::idForTrack (base, track), value.number);
        }
    }
    for (const auto& [pid, raw] : values)
    {
        const auto index = params_.indexOf (pid);
        if (index == ParameterSet::npos) continue;
        const double v = params_.spec (index).clamp (raw);
        if (params_.getAt (index) == v) continue;
        undo_.setParameter (params_, pid, v);
        if (auto* hp = hostParam (index))
        {
            hp->beginChangeGesture();
            hp->setValueNotifyingHost (static_cast<float> (params_.getNormalizedAt (index)));
            hp->endChangeGesture();
        }
    }
    if (! p->facade.empty()) // un preset sans Façade laisse celle de la piste ; avec une Façade, elle la remplace (même entrée d'annulation)
    {
        facade::Facade loaded;
        if (facade_bundle::restoreFromFolder (p->facade, track, facade_bundle::presetAssetsFolder (presetLibrary_->root()), loaded))
        {
            auto strip = trackSettings (track);
            strip.facade = std::move (loaded);
            uiEditTrack (track, std::move (strip), "Preset");
        }
    }
    undo_.endTransaction();
    control_.resetPickup(); // les assignations « reprise » attendent de nouveau le passage de la source
    setTrackPresetId (track, id);
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged (false).withNonParameterStateChanged (true));
    return true;
}

bool NeptuneProcessor::stepPreset (int direction)
{
    const auto next = presetLibrary_->stepFrom (currentPresetId(), direction,
                                                [this] (const neptune::core::preset::Preset& p) { return acceptsPreset (p); });
    return ! next.empty() && loadPreset (next);
}

bool NeptuneProcessor::isModified() const
{
    using namespace neptune::core;
    const auto* p = currentPreset();
    if (p == nullptr) return false;
    const int track = selectedTrack();
    if (trackType (track) == TrackType::Organ) return organ::differsFromPreset (*p, params_, presetLibrary_->config(), track);
    if (trackType (track) == TrackType::Fm)
        return fm::differsFromPreset (*p, params_, fmDescription (track), presetLibrary_->config(), track);
    return analogsynth::differsFromPreset (*p, params_, synthDescription (track), presetLibrary_->config(), track);
}

bool NeptuneProcessor::canSaveCurrent() const
{
    const auto* p = currentPreset();
    return p != nullptr && ! p->factory && ! p->locked;
}

bool NeptuneProcessor::overwritePreset (const std::string& id, std::string* error)
{
    using namespace neptune::core;
    const auto* base = presetLibrary_->find (id);
    if (base == nullptr || base->factory) { if (error) *error = "factory preset is read-only"; return false; }
    if (base->locked) { if (error) *error = "preset is locked"; return false; }
    const int track = selectedTrack();
    preset::Preset p = trackType (track) == TrackType::Organ
        ? organ::capturePreset (params_, presetLibrary_->config(), base->name, base->type, base, track)
        : trackType (track) == TrackType::Fm
        ? fm::capturePreset (params_, fmDescription (track), presetLibrary_->config(), base->name, base->type, base, track)
        : analogsynth::capturePreset (params_, synthDescription (track), presetLibrary_->config(), base->name, base->type, base, track);
    attachFacade (p, track);
    const bool ok = presetLibrary_->save (std::move (p), error);
    if (ok) ++presetRevision_;
    return ok;
}

// La Façade de la piste entre dans le preset (images copiées dans la bibliothèque) ; une piste sans Façade n'en met pas.
void NeptuneProcessor::attachFacade (neptune::core::preset::Preset& p, int track) const
{
    const auto f = trackSettings (track).facade;
    if (f == neptune::core::facade::Facade()) { p.facade.clear(); return; }
    facade_bundle::SaveReport report;
    p.facade = facade_bundle::storeInFolder (f, track, facade_bundle::presetAssetsFolder (presetLibrary_->root()), report);
    p = neptune::core::preset::sanitised (std::move (p));
}

bool NeptuneProcessor::saveCurrent (std::string* error)
{
    const auto id = currentPresetId();
    if (id.empty()) { if (error) *error = "no current preset"; return false; }
    return overwritePreset (id, error);
}

bool NeptuneProcessor::saveCurrentAs (const std::string& name, const std::string& type, neptune::core::preset::Preset* out, std::string* error)
{
    using namespace neptune::core;
    const int track = selectedTrack();
    const auto tt = trackType (track);
    if (tt != TrackType::Synth && tt != TrackType::Fm && tt != TrackType::Organ) { if (error) *error = "not a synth, FM or organ track"; return false; }
    auto capture = [&] (const preset::Preset* base, const std::string& n, const std::string& ty)
    {
        if (tt == TrackType::Organ) return organ::capturePreset (params_, presetLibrary_->config(), n, ty, base, track);
        return tt == TrackType::Fm ? fm::capturePreset (params_, fmDescription (track), presetLibrary_->config(), n, ty, base, track)
                                   : analogsynth::capturePreset (params_, synthDescription (track), presetLibrary_->config(), n, ty, base, track);
    };
    auto p = capture (nullptr, name, type);
    if (const auto* cur = currentPreset())
    {
        p.characteristics = cur->characteristics;
        p.customFields = cur->customFields;
        p.rating = cur->rating;
        p.captureOverrides = cur->captureOverrides;
        if (p.type.empty()) p.type = cur->type;
        p.derivedFrom = cur->factory ? cur->id : cur->derivedFrom;
        p = capture (&p, name, p.type);
    }
    attachFacade (p, track);
    preset::Preset stored;
    if (! presetLibrary_->importPreset (std::move (p), &stored, error)) return false;
    setTrackPresetId (track, stored.id);
    if (out != nullptr) *out = stored;
    return true;
}

bool NeptuneProcessor::saveConfig (const neptune::core::preset::LibraryConfig& c)
{
    const bool ok = presetLibrary_->setConfig (c);
    if (ok) ++presetRevision_;
    return ok;
}

void NeptuneProcessor::pushMixerSettingsToEngine()
{
    const auto m = mixerState();
    for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
    {
        const auto& ts = m.tracks[static_cast<std::size_t> (t)];
        engine_.setTrackSettings (t, ts.strip);
        engine_.setTrackType (t, ts.type);
        engine_.setTrackActive (t, ts.active);
        publishSynthPatch (t, ts.synth);
        publishFmDescription (t, ts.fm);
        publishOrganDescription (t, ts.organ);
    }
    for (int b = 0; b < neptune::core::mixer::numBuses; ++b) engine_.setBusSettings (b, m.buses[static_cast<std::size_t> (b)]);
    for (int p = 0; p < neptune::core::mixer::numParallels; ++p)
    {
        const auto& ps = m.parallels[static_cast<std::size_t> (p)];
        engine_.setParallelSettings (p, ps.strip);
        engine_.setParallel (p, ps.active, ps.parent, ps.tap, ps.followParentVolume);
    }
    engine_.setMasterSettings (m.master);
}

namespace
{
using neptune::core::mixer::StripSettings;
}

void NeptuneProcessor::uiEditTrack (int track, StripSettings s, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    track = juce::jlimit (0, mixer::numTracks - 1, track);
    s = mixer::sanitisedStrip (std::move (s), false, track);
    const auto previous = trackSettings (track);
    if (previous == s) return;
    undo_.perform (std::make_unique<SnapshotCommand<StripSettings>> (
        previous, s,
        [this, track] (const StripSettings& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.tracks[static_cast<std::size_t> (track)].strip = v; }
            engine_.setTrackSettings (track, v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

void NeptuneProcessor::uiEditBus (int bus, StripSettings s, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    bus = juce::jlimit (0, mixer::numBuses - 1, bus);
    s = mixer::sanitisedStrip (std::move (s), true, bus);
    const auto previous = busSettings (bus);
    if (previous == s) return;
    undo_.perform (std::make_unique<SnapshotCommand<StripSettings>> (
        previous, s,
        [this, bus] (const StripSettings& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.buses[static_cast<std::size_t> (bus)] = v; }
            engine_.setBusSettings (bus, v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

void NeptuneProcessor::uiEditMaster (neptune::core::mixer::MasterSettings m, std::string name, std::string mergeKey)
{
    using namespace neptune::core;
    m = mixer::sanitised (m);
    const auto previous = masterSettings();
    if (previous == m) return;
    undo_.perform (std::make_unique<SnapshotCommand<mixer::MasterSettings>> (
        previous, m,
        [this] (const mixer::MasterSettings& v)
        {
            { const juce::ScopedLock lock (instrumentLock_); mixer_.master = v; }
            engine_.setMasterSettings (v);
            ++revision_;
            notifyStateChanged();
        },
        std::move (name), std::move (mergeKey)));
}

// Chargement d'un état (projet ou autosave) : ne marque pas le projet modifié.
void NeptuneProcessor::adoptMixerFromState (const std::optional<neptune::core::MixerState>& m)
{
    if (! m) return;
    std::array<bool, neptune::core::mixer::numTracks> changed {};
    {
        const juce::ScopedLock lock (instrumentLock_);
        for (std::size_t t = 0; t < mixer_.tracks.size(); ++t)
        {
            changed[t] = ! (mixer_.tracks[t].instrument == m->tracks[t].instrument);
            mixer_.tracks[t] = m->tracks[t];
        }
        mixer_.buses = m->buses;
        mixer_.parallels = m->parallels;
        mixer_.master = m->master;
        mixer_.router = m->router;
        mixer_.combinations = m->combinations;
        mixer_.console = m->console;
        mixer_.globalFacade = m->globalFacade;
        mixer_.selectedTrack = m->selectedTrack;
    }
    engine_.setRouter (m->router);
    combi_.resetSequence();
    selectedTrack_.store (m->selectedTrack);
    pushMixerSettingsToEngine();
    undo_.clear(); // l'historique pointait vers les modules et les instruments remplacés
    {
        namespace mx = neptune::core::mixer;
        auto build = [this] (StripKind kind, int index, const std::array<mx::InsertState, mx::numInserts>& states)
        {
            for (int s = 0; s < mx::numInserts; ++s)
            {
                std::shared_ptr<neptune::core::InsertInstance> inst;
                const auto& st = states[static_cast<std::size_t> (s)];
                if (! st.empty())
                {
                    inst = neptune::core::createInsert (st.type, engine_.sampleRate(), engine_.maxBlockSize());
                    if (inst != nullptr)
                    {
                        for (const auto& [id, v] : st.values) inst->runner.params().set (id, v);
                        if (! st.text.empty() && inst->runner.module().hasStateString()) inst->runner.module().setStateString (st.text);
                    }
                }
                {
                    const juce::ScopedLock lock (instrumentLock_);
                    insertInstances_[static_cast<std::size_t> (insertFlatIndex (kind, index))][static_cast<std::size_t> (s)] = inst;
                }
                engine_.setInsert (kind, index, s, inst);
            }
        };
        for (int t = 0; t < mx::numTracks; ++t) build (StripKind::Track, t, m->tracks[static_cast<std::size_t> (t)].strip.inserts);
        for (int b = 0; b < mx::numBuses; ++b) build (StripKind::Bus, b, m->buses[static_cast<std::size_t> (b)].inserts);
        build (StripKind::Master, 0, m->master.inserts);
        for (int p = 0; p < mx::numParallels; ++p) build (StripKind::Parallel, p, m->parallels[static_cast<std::size_t> (p)].strip.inserts);
    }
    ++revision_;
    for (std::size_t t = 0; t < m->tracks.size(); ++t)
        if (changed[t]) loader_->request (static_cast<int> (t), m->tracks[t].instrument, false);
}

NeptuneProcessor::BusesProperties NeptuneProcessor::makeBuses (int numOutputs)
{
    BusesProperties props;
    props = props.withInput ("Input", juce::AudioChannelSet::stereo(), true);   // alimente les pistes « Entrée audio » (réglage par piste)
    for (int i = 0; i < numOutputs; ++i)
        props = props.withOutput ("Out " + juce::String (i + 1), juce::AudioChannelSet::stereo(), true);
    return props;
}

NeptuneProcessor::NeptuneProcessor()
    : juce::AudioProcessor (makeBuses (NEPTUNE_PROBE_OUTPUTS)),
      probeTracks_ (NEPTUNE_PROBE_TRACKS),
      probeOutputs_ (NEPTUNE_PROBE_OUTPUTS)
{
    std::string error;
    const bool ok = neptune::core::probe::buildPool (params_, probeTracks_, &error);
    jassert (ok);
    const bool samplerOk = neptune::core::sampler::addParams (params_, &error);
    jassert (samplerOk);
    const bool mixerOk = neptune::core::mixer::addParams (params_, probeTracks_, neptune::core::mixer::numBuses, &error);
    jassert (mixerOk);
    const bool macrosOk = neptune::core::control::addMacroParams (params_, &error);
    jassert (macrosOk);
    juce::ignoreUnused (ok, samplerOk, mixerOk, macrosOk);
    macroFirst_ = params_.indexOf (neptune::core::control::macroParamId (0));
    engine_.attachParameters (&params_);
    // une touche de keyswitch bascule un modulateur : la valeur est écrite comme par la couche de contrôle (relayée vers l'hôte)
    engine_.setModulatorWriter ([this] (neptune::core::ParamId id, double value) { control_.setParameter (id, value, true, params_); });
    mixer_ = neptune::core::MixerState::freshProject(); // projet neuf : une seule piste, un synthé
    pushMixerSettingsToEngine();

    // Réglages globaux : lus une fois au démarrage ; le fichier est créé avec les valeurs par défaut s'il manque.
    const auto configDir = configDirectory();
    settingsFile_ = configDir / "settings.json";
    std::vector<std::string> warnings;
    settings_ = neptune::core::settings::loadFile (settingsFile_, &warnings);
    for (const auto& w : warnings)
        DBG ("Neptune settings: " << juce::String (w));

    // Paramètres déclarés à l'hôte (il lit la liste une fois) : tous, ou seulement les bandes et les macros (réglage global, lu ici).
    hostIndex_.assign (params_.size(), -1);
    for (std::size_t i = 0; i < params_.size(); ++i)
    {
        if (settings_.reducedHostList && ! neptune::core::control::isBandOrMacroParam (params_.spec (i).id)) continue;
        hostIndex_[i] = getParameters().size();
        addParameter (new NeptuneParameter (params_, i, hostProbe_)); // l'AudioProcessor en prend la propriété
    }
    loader_ = std::make_unique<InstrumentLoader> (*this);
    stylelib::setDirectory (juce::File (settingsFilePath()).getParentDirectory().getChildFile ("styles"));   // packs de styles : aussi sans éditeur ouvert (presets, export de piste)
    {
        // Presets de synthé : la même bibliothèque qu'AnalogSynth (dossier choisi dans les réglages, sinon <config>/AnalogSynth).
        auto root = settings_.presetsDir.empty() ? configDir / "AnalogSynth" : std::filesystem::u8path (settings_.presetsDir);
        presetLibrary_ = std::make_unique<neptune::core::preset::Library> (std::move (root), [] { auto all = neptune::core::analogsynth::factoryPresets(); for (auto& f : neptune::core::fm::factoryPresets()) all.push_back (std::move (f)); for (auto& f : neptune::core::organ::factoryPresets()) all.push_back (std::move (f)); return all; }());
        presetLibrary_->open (&warnings);
        if (presetLibrary_->config().parameterCaptureDefaults.empty()) presetLibrary_->setConfig (neptune::core::analogsynth::defaultConfig());
        {
            // la vitesse du Leslie est libre par défaut (configuration partagée avec les autres plugins : on complète)
            auto cfg = presetLibrary_->config();
            if (cfg.parameterCaptureDefaults.count (neptune::core::organ::leslieSpeedKey) == 0)
            {
                cfg.parameterCaptureDefaults[neptune::core::organ::leslieSpeedKey] = neptune::core::preset::Capture::Free;
                presetLibrary_->setConfig (cfg);
            }
        }
    }
    uiStateFile_ = configDir / "ui_state.json";
    uiState_ = neptune::core::uistate::loadFile (uiStateFile_);

    // Première instance du processus : le profil global vient de settings.json ; les suivantes le partagent.
    neptune::core::SharedProfile::process().initialiseOnce (settings_.profile);
    std::error_code ec;
    if (! std::filesystem::exists (settingsFile_, ec))
        neptune::core::settings::saveFile (settingsFile_, settings_);

    // Autosave : fichiers orphelins trop anciens supprimés, puis écriture périodique de l'état s'il a changé.
    const auto autosaveDir = configDir / "autosave";
    neptune::core::AutosaveStore::purgeStale (autosaveDir, settings_.autosaveKeepDays);
    autosave_ = std::make_unique<neptune::core::AutosaveStore> (autosaveDir);
    autosave_->setBaseline (currentStateText());

    if (settings_.autosaveIntervalSeconds > 0)
    {
        autosaveTimer_.onTick = [this] { autosaveNow(); };
        autosaveTimer_.startTimer (settings_.autosaveIntervalSeconds * 1000);
    }

    control_.configure (neptune::core::control::Config {}, params_); // CC et OSC désactivés tant que l'utilisateur ne les active pas
    controlTimer_.onTick = [this] { controlTick(); };
    controlTimer_.startTimer (33);
}

NeptuneProcessor::~NeptuneProcessor()
{
    controlTimer_.stopTimer();
    osc_.stop();
    autosaveTimer_.stopTimer();
    autosave_->discard(); // fermeture propre : il n'y a rien à récupérer
}

void NeptuneProcessor::uiSetControlConfig (neptune::core::control::Config c)
{
    control_.configure (c, params_);
    notifyStateChanged();
}

void NeptuneProcessor::uiSetCcEnabled (bool on)
{
    auto c = control_.config();
    if (c.settings.ccEnabled == on) return;
    c.settings.ccEnabled = on;
    uiSetControlConfig (std::move (c));
}

bool NeptuneProcessor::uiAssignOrganControllers()
{
    using namespace neptune::core;
    const int track = selectedTrack();
    if (trackType (track) != TrackType::Organ) return false;
    auto c = control_.config();
    // les assignations de cette piste d'orgue sont remplacées, celles des autres pistes et des autres cibles restent
    c.bindings.erase (std::remove_if (c.bindings.begin(), c.bindings.end(),
                                      [track] (const control::Binding& b) { return ! b.target.isAction && organ::isOrganParam (b.target.param, track); }),
                      c.bindings.end());
    for (auto& b : organ::organControlBindings (organDescription (track), track)) c.bindings.push_back (std::move (b));
    uiSetControlConfig (std::move (c));
    return true;
}

void NeptuneProcessor::uiClearControllers()
{
    auto c = control_.config();
    c.bindings.clear();
    uiSetControlConfig (std::move (c));
}

// Relais du thread de message : actions demandées par une source (preset suivant, panique...) et paramètres modifiés par une source
// externe, signalés à l'hôte (regroupés : une notification par paramètre et par tic).
bool NeptuneProcessor::canAssignMacro (neptune::core::ParamId id) const
{
    using namespace neptune::core;
    return params_.find (id) != nullptr && ! (id >= control::kMacroBase && id < control::kMacroBase + static_cast<ParamId> (control::kMacros));
}

std::vector<int> NeptuneProcessor::macrosOf (neptune::core::ParamId id) const
{
    using namespace neptune::core::control;
    std::vector<int> out;
    for (const auto& b : control_.config().bindings)
        if (b.source.kind == SourceKind::Macro && ! b.target.isAction && b.target.param == id) out.push_back (b.source.number);
    return out;
}

void NeptuneProcessor::uiToggleMacro (neptune::core::ParamId id, int macro)
{
    using namespace neptune::core::control;
    if (! canAssignMacro (id) || macro < 0 || macro >= kMacros) return;
    auto c = control_.config();
    const auto before = c.bindings.size();
    c.bindings.erase (std::remove_if (c.bindings.begin(), c.bindings.end(), [&] (const Binding& b)
                      { return b.source.kind == SourceKind::Macro && b.source.number == macro && ! b.target.isAction && b.target.param == id; }), c.bindings.end());
    if (c.bindings.size() == before)
    {
        Binding b;
        b.source.kind = SourceKind::Macro;
        b.source.number = macro;
        b.target.param = id;
        c.bindings.push_back (std::move (b));
    }
    uiSetControlConfig (std::move (c));
}

void NeptuneProcessor::uiClearMacros (neptune::core::ParamId id)
{
    using namespace neptune::core::control;
    auto c = control_.config();
    c.bindings.erase (std::remove_if (c.bindings.begin(), c.bindings.end(), [&] (const Binding& b)
                      { return b.source.kind == SourceKind::Macro && ! b.target.isAction && b.target.param == id; }), c.bindings.end());
    uiSetControlConfig (std::move (c));
}

std::string NeptuneProcessor::uiText (std::string_view key) const
{
    return std::string (neptune::core::i18n::Translator (settings_.language).tr (key));
}

void NeptuneProcessor::uiSetReducedHostList (bool on)
{
    if (settings_.reducedHostList == on) return;
    settings_.reducedHostList = on;
    std::string error;
    neptune::core::settings::saveFile (settingsFile_, settings_, &error);   // lu au prochain chargement du plugin
}

void NeptuneProcessor::uiSetLook (const std::string& theme, int contrast, const std::string& language)
{
    settings_.theme = theme;
    settings_.contrast = juce::jlimit (0, 4, contrast);
    settings_.language = language;
    std::string error;
    neptune::core::settings::saveFile (settingsFile_, settings_, &error);   // la bascule du profil Live / Studio, elle, n'est jamais écrite ici : settings_.profile reste celui du démarrage
}

void NeptuneProcessor::uiSetShowDiagnostics (bool on)
{
    if (settings_.showDiagnostics == on) return;
    settings_.showDiagnostics = on;
    std::string error;
    neptune::core::settings::saveFile (settingsFile_, settings_, &error);
}

void NeptuneProcessor::uiSetMixerZoomPercent (int percent)
{
    percent = std::clamp (percent, 60, 160);
    if (settings_.mixerZoomPercent == percent) return;
    settings_.mixerZoomPercent = percent;
    std::string error;
    neptune::core::settings::saveFile (settingsFile_, settings_, &error);
}

void NeptuneProcessor::syncOsc()
{
    const auto now = control_.settings();
    const bool wanted = now.oscEnabled;
    if (wanted == oscRunning_ && (! wanted || (now.oscPort == oscApplied_.oscPort && now.oscAllInterfaces == oscApplied_.oscAllInterfaces))) return;
    if (! wanted) osc_.stop();
    else osc_.start (now.oscPort, now.oscAllInterfaces);   // 0 si aucun port libre : le menu l'indique
    oscRunning_ = wanted;
    oscApplied_ = now;
}

// Retour d'information OSC : toute valeur qui change (interface, hôte, CC, OSC) est renvoyée au destinataire, regroupée à ~30 Hz. Un « /neptune/sync »
// reçu, ou un nouveau destinataire, renvoie l'état complet (par tranches, pour ne pas saturer le réseau).
void NeptuneProcessor::feedbackTick()
{
    const auto settings = control_.settings();
    if (oscRunning_)
    {
        const juce::String manual = settings.feedbackPort > 0 ? juce::String (settings.feedbackHost) : juce::String();
        osc_.setManualDestination (manual, settings.feedbackPort);
    }
    if (! oscRunning_ || ! osc_.hasDestination())
    {
        feedbackVersions_.clear();
        feedbackPending_.clear();
        feedbackHead_ = 0;
        osc_.takeFullSendRequest();
        return;
    }
    const bool full = osc_.takeFullSendRequest();
    if (feedbackVersions_.empty() || full)
    {
        // première activation ou demande de synchronisation : tout l'état part
        feedbackVersions_.assign (params_.size(), 0);
        for (std::size_t i = 0; i < params_.size(); ++i) feedbackVersions_[i] = params_.versionAt (i);
        feedbackPending_.clear();
        feedbackHead_ = 0;
        for (std::size_t i = 0; i < params_.size(); ++i) feedbackPending_.push_back (i);
    }
    else
        for (std::size_t i = 0; i < params_.size(); ++i)
        {
            const auto v = params_.versionAt (i);
            if (v == feedbackVersions_[i]) continue;
            feedbackVersions_[i] = v;
            feedbackPending_.push_back (i);
        }
    if (feedbackHead_ >= feedbackPending_.size())
    {
        feedbackPending_.clear();
        feedbackHead_ = 0;
        return;
    }
    constexpr std::size_t kPerTick = 400;
    const auto end = std::min (feedbackPending_.size(), feedbackHead_ + kPerTick);
    const std::vector<std::size_t> chunk (feedbackPending_.begin() + static_cast<std::ptrdiff_t> (feedbackHead_), feedbackPending_.begin() + static_cast<std::ptrdiff_t> (end));
    feedbackHead_ = end;
    osc_.send (neptune::core::osc::buildFeedback (params_, chunk));
}

bool NeptuneProcessor::uiSetOscFeedback (const juce::String& hostAndPort)
{
    auto c = control_.config();
    const auto text = hostAndPort.trim();
    if (text.isEmpty())
    {
        c.settings.feedbackHost.clear();
        c.settings.feedbackPort = 0;
        osc_.stopFeedback();
    }
    else
    {
        const int colon = text.lastIndexOfChar (':');
        const auto host = colon > 0 ? text.substring (0, colon).trim() : juce::String();
        const int port = colon > 0 ? text.substring (colon + 1).trim().getIntValue() : 0;
        if (host.isEmpty() || host.length() > 253 || port < 1 || port > 65535) return false;
        c.settings.feedbackHost = host.toStdString();
        c.settings.feedbackPort = port;
    }
    uiSetControlConfig (std::move (c));
    syncOsc();
    return true;
}

void NeptuneProcessor::uiSetOscEnabled (bool on)
{
    auto c = control_.config();
    if (c.settings.oscEnabled == on) return;
    c.settings.oscEnabled = on;
    uiSetControlConfig (std::move (c));
    syncOsc();
}

void NeptuneProcessor::uiSetOscAllInterfaces (bool on)
{
    auto c = control_.config();
    if (c.settings.oscAllInterfaces == on) return;
    c.settings.oscAllInterfaces = on;
    uiSetControlConfig (std::move (c));
    syncOsc();
}

void NeptuneProcessor::refreshLatency()
{
    engine_.setProfile (profile_.effective());   // le moteur ne le reçoit sinon qu'au premier bloc audio
    const int latency = engine_.updateLatencyPlan();
    if (latency != declaredLatency_)
    {
        declaredLatency_ = latency;
        setLatencySamples (latency);
    }
}

void NeptuneProcessor::controlTick()
{
    using neptune::core::control::Action;
    control_.collectGarbage();
    if (++latencyTicks_ >= 6)   // environ 5 fois par seconde : le plan de compensation suit les inserts, le routage et le profil
    {
        latencyTicks_ = 0;
        refreshLatency();
    }
    syncOsc();   // un état chargé par l'hôte peut avoir activé ou arrêté le serveur
    feedbackTick();
    drainCombinationPrograms();
    for (Action a = Action::None; control_.popAction (a);)
    {
        switch (a)
        {
            case Action::PresetNext: stepPreset (1); break;
            case Action::PresetPrevious: stepPreset (-1); break;
            case Action::Panic: uiPanic(); break;
            case Action::TrackNext:
            case Action::TrackPrevious:
            {
                const int step = a == Action::TrackNext ? 1 : -1;
                const int n = neptune::core::mixer::numTracks;
                for (int k = 1; k < n; ++k)
                {
                    const int t = ((selectedTrack() + step * k) % n + n) % n;
                    if (trackActive (t)) { selectTrack (t); break; }
                }
                break;
            }
            default:
                // CC, note ou OSC lié à une commande de combinaison : les divisionnels visent le clavier (la piste) sélectionné
                if (const int command = neptune::core::control::combinationCommand (a); command > 0) uiCombiCommand (command, selectedTrack());
                break;
        }
    }
    std::vector<std::size_t> changed;
    if (control_.drainChanges (changed) == 0) return;
    for (const auto index : changed)
        if (auto* p = hostParam (index))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (static_cast<float> (params_.getNormalizedAt (index)));
            p->endChangeGesture();
        }
}

std::string NeptuneProcessor::currentStateText() const
{
    const auto mixer = mixerState();
    const auto control = neptune::core::control::toJson (control_.config());
    return neptune::core::state::save (params_, autosave_->id(), profile_.locked(), &mixer, control);
}

// Changement d'état qui n'est pas un paramètre (le verrou de profil) : le wrapper CLAP le traduit en
// clap_host_state.mark_dirty, ce qui fait marquer le projet comme modifié par l'hôte.
void NeptuneProcessor::notifyStateChanged()
{
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
}

void NeptuneProcessor::uiBeginEdit (neptune::core::ParamId id)
{
    const auto index = params_.indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return;
    if (auto* hp = hostParam (index)) hp->beginChangeGesture();
    undo_.beginGesture();
}

void NeptuneProcessor::uiSetNormalised (neptune::core::ParamId id, double normalised)
{
    const auto index = params_.indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return;
    // D'abord le cœur par la pile d'annulation (elle lit l'ancienne valeur), puis l'hôte est prévenu.
    undo_.setParameter (params_, id, params_.spec (index).fromNormalized (normalised));
    if (auto* hp = hostParam (index)) hp->setValueNotifyingHost (static_cast<float> (params_.getNormalizedAt (index)));
}

void NeptuneProcessor::uiEndEdit (neptune::core::ParamId id)
{
    const auto index = params_.indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return;
    undo_.endGesture();
    if (auto* hp = hostParam (index)) hp->endChangeGesture();
}

void NeptuneProcessor::uiResetToDefault (neptune::core::ParamId id)
{
    const auto index = params_.indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return;
    uiBeginEdit (id);
    uiSetNormalised (id, params_.spec (index).toNormalized (params_.spec (index).defaultValue));
    uiEndEdit (id);
}

// Annuler/rétablir changent des valeurs hors des événements de paramètres : l'hôte les relit (comme au chargement d'un état).
bool NeptuneProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done)
        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

bool NeptuneProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done)
        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

void NeptuneProcessor::setLastImportTemplate (const std::string& json)
{
    if (json == uiState_.lastImportTemplate) return;
    uiState_.lastImportTemplate = json;
    std::string error;
    if (! neptune::core::uistate::saveFile (uiStateFile_, uiState_, &error))
        DBG ("Neptune ui state not saved: " << juce::String (error));
}

void NeptuneProcessor::setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s)
{
    auto next = uiState_;
    next.setModalSize (t, s);
    if (next == uiState_) return;
    uiState_ = next;
    std::string error;
    if (! neptune::core::uistate::saveFile (uiStateFile_, uiState_, &error))
        DBG ("Neptune ui state not saved: " << juce::String (error));
}

void NeptuneProcessor::autosaveNow()
{
    std::string error;
    if (! autosave_->write (currentStateText(), &error))
        DBG ("Neptune autosave failed: " << juce::String (error));
}

// L'état du projet porte l'identifiant de l'instance de la session précédente : on le reprend pour retrouver
// son autosave. Le fichier laissé par un crash est mis de côté et proposé à l'utilisateur.
void NeptuneProcessor::adoptStateIdentity (const std::string& idFromState)
{
    if (! idFromState.empty())
        autosave_->adoptId (idFromState); // refusé si invalide ou déjà pris : on garde l'identifiant courant

    const std::string loaded = currentStateText();
    autosave_->setBaseline (loaded);

    auto rec = autosave_->takeRecoverable();
    if (! rec) return;
    if (rec->state == loaded) // identique à ce que le projet vient de charger : rien à proposer
    {
        autosave_->resolveRecovered();
        return;
    }
    const juce::ScopedLock lock (recoveryLock_);
    recovered_ = std::move (rec);
}

bool NeptuneProcessor::hasRecoverableState() const
{
    const juce::ScopedLock lock (recoveryLock_);
    return recovered_.has_value();
}

juce::String NeptuneProcessor::recoverableDescription() const
{
    const juce::ScopedLock lock (recoveryLock_);
    if (! recovered_) return {};
    return juce::Time (static_cast<juce::int64> (recovered_->savedAt) * 1000).toString (true, true);
}

void NeptuneProcessor::restoreRecovered()
{
    std::optional<neptune::core::RecoveredState> rec;
    {
        const juce::ScopedLock lock (recoveryLock_);
        rec = std::move (recovered_);
        recovered_.reset();
    }
    if (! rec) return;

    const auto result = neptune::core::state::load (params_, rec->state);
    if (! result.ok())
    {
        DBG ("Neptune: recovered state not loaded: " << juce::String (result.message));
    }
    else
    {
        profile_.setLocked (result.profileLock);
        macroResync_.store (true);
        control_.configure (result.controlJson.empty() ? neptune::core::control::Config {} : neptune::core::control::configFromJson (result.controlJson), params_);
        adoptMixerFromState (result.mixer);
    }
    autosave_->resolveRecovered();
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    autosaveNow();
}

void NeptuneProcessor::discardRecovered()
{
    {
        const juce::ScopedLock lock (recoveryLock_);
        recovered_.reset();
    }
    autosave_->resolveRecovered();
}

void NeptuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    refreshLatency();   // à l'activation, sans attendre la minuterie (un hôte sans boucle d'événements ne la verrait jamais)
    const auto capacity = static_cast<std::size_t> (juce::jmax (samplesPerBlock, 8192));
    hostIn_[0].assign (capacity, 0.0f);
    hostIn_[1].assign (capacity, 0.0f);
}

bool NeptuneProcessor::uiStartRecording (std::string& path, std::string* error)
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S").toStdString();
    const auto file = configDirectory() / "Edited" / "Recordings" / ("rec-" + stamp + ".wav");
    if (! recorder_.start (file, getSampleRate() > 0.0 ? getSampleRate() : 48000.0, error)) return false;
    path = file.u8string();
    return true;
}

void NeptuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    engine_.setProfile (profile_.effective()); // lecture atomique, sans verrou
    // Entrée audio de l'hôte : copiée avant tout (les canaux 0 et 1 du tampon sont aussi la première sortie, que le moteur va écrire)
    {
        const auto* inBus = getBus (true, 0);
        const int frames = buffer.getNumSamples();
        if (inBus != nullptr && inBus->isEnabled() && inBus->getNumberOfChannels() >= 1 && frames > 0
            && static_cast<std::size_t> (frames) <= hostIn_[0].size() && hostIn_[1].size() >= hostIn_[0].size())
        {
            const int channels = inBus->getNumberOfChannels();
            std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + frames, hostIn_[0].begin());
            const float* right = buffer.getReadPointer (channels >= 2 ? 1 : 0);   // entrée mono : dupliquée
            std::copy (right, right + frames, hostIn_[1].begin());
            engine_.setHostInput (hostIn_[0].data(), hostIn_[1].data(), frames);
            recorder_.push (hostIn_[0].data(), hostIn_[1].data(), frames);   // sans effet hors enregistrement
        }
    }
    ++hostProbe_.blocks;
    hostProbe_.lastBlockSize = buffer.getNumSamples();
    hostProbe_.outputChannels = buffer.getNumChannels();
    if (auto* head = getPlayHead())
    {
        if (const auto pos = head->getPosition())
        {
            hostProbe_.hasTransport = true;
            hostProbe_.playing = pos->getIsPlaying();
            hostProbe_.bpm = pos->getBpm().orFallback (0.0);
            hostProbe_.ppq = pos->getPpqPosition().orFallback (0.0);
            if (const auto sig = pos->getTimeSignature())
            {
                hostProbe_.timeSigNum = sig->numerator;
                hostProbe_.timeSigDen = sig->denominator;
            }
        }
        else
            hostProbe_.hasTransport = false;
    }

    // messages MIDI envoyés à la main : vers Neptune, ils rejoignent le tampon comme s'ils venaient d'un clavier ; vers l'hôte, ils attendent la fin du bloc
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        midiFifo_.prepareToRead (midiFifo_.getNumReady(), start1, size1, start2, size2);
        auto handle = [this, &midi] (const MidiPacket& p)
        {
            if (p.size == 0) return;
            if ((p.targets & 1) != 0) midi.addEvent (juce::MidiMessage (p.bytes.data(), static_cast<int> (p.size)), 0);
            if ((p.targets & 2) != 0 && numHostMidi_ < static_cast<int> (hostMidi_.size())) hostMidi_[static_cast<std::size_t> (numHostMidi_++)] = p;
        };
        for (int i = 0; i < size1; ++i) handle (midiPackets_[static_cast<std::size_t> (start1 + i)]);
        for (int i = 0; i < size2; ++i) handle (midiPackets_[static_cast<std::size_t> (start2 + i)]);
        midiFifo_.finishedRead (size1 + size2);
    }

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
        {
            ++hostProbe_.midiNoteOns;
            const int ch = (msg.getChannel() - 1) & 15;
            ++heldPerChannel_[ch];
            midiLevel_[ch].store (msg.getFloatVelocity(), std::memory_order_relaxed);
        }
        else if (msg.isNoteOff())
        {
            const int ch = (msg.getChannel() - 1) & 15;
            if (heldPerChannel_[ch] > 0 && --heldPerChannel_[ch] == 0) midiLevel_[ch].store (0.0f, std::memory_order_relaxed);
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            for (int ch = 0; ch < 16; ++ch) { heldPerChannel_[ch] = 0; midiLevel_[ch].store (0.0f, std::memory_order_relaxed); }
        if (msg.isAllSoundOff())
            engine_.panic();
        else if (msg.isAllNotesOff())
            engine_.releaseAll();
        else if (msg.isNoteOn())
            engine_.noteOn (msg.getChannel() - 1, msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            engine_.noteOff (msg.getChannel() - 1, msg.getNoteNumber());
        else if (msg.isController())
            control_.controlChange (msg.getChannel() - 1, msg.getControllerNumber(), msg.getControllerValue(), params_); // désactivé par défaut
        else if (msg.isProgramChange())
        {
            // les combinaisons d'orgue (général / divisionnel / banque) : exécutées sur le fil de message, par controlTick
            int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
            combiProgramFifo_.prepareToWrite (1, s1, z1, s2, z2);
            if (z1 + z2 >= 1)
            {
                combiPrograms_[static_cast<std::size_t> (z1 > 0 ? s1 : s2)].store (((msg.getChannel() - 1) & 15) << 8 | (msg.getProgramChangeNumber() & 127), std::memory_order_relaxed);
                combiProgramFifo_.finishedWrite (1);
            }
        }

        // dernier état des contrôleurs de chaque canal : jugé par les conditions MIDI des zones au moment où une note se déclenche
        {
            neptune::core::midi::Message mm;
            mm.channel = (msg.getChannel() - 1) & 15;
            bool known = true;
            if (msg.isController()) { mm.kind = neptune::core::midi::MsgKind::ControlChange; mm.data1 = msg.getControllerNumber(); mm.data2 = msg.getControllerValue(); }
            else if (msg.isPitchWheel())
            {
                mm.kind = neptune::core::midi::MsgKind::PitchBend;
                mm.data2 = msg.getPitchWheelValue() >> 7;
                engine_.pitchBendMidi (mm.channel, msg.getPitchWheelValue());   // les pistes Synthé et FM suivent la molette sur 14 bits
            }
            else if (msg.isChannelPressure()) { mm.kind = neptune::core::midi::MsgKind::ChannelPressure; mm.data2 = msg.getChannelPressureValue(); }
            else if (msg.isAftertouch()) { mm.kind = neptune::core::midi::MsgKind::PolyPressure; mm.data1 = msg.getNoteNumber(); mm.data2 = msg.getAfterTouchValue(); }
            else if (msg.isProgramChange()) { mm.kind = neptune::core::midi::MsgKind::ProgramChange; mm.data1 = msg.getProgramChangeNumber(); }
            else known = false;
            if (known) engine_.midiController (mm);
        }
    }

    // Macros : une macro dont la valeur a changé (automation de l'hôte, interface, OSC) écrit les cibles qui lui sont assignées. Après le
    // chargement d'un état, on mémorise les valeurs sans rien appliquer (les cibles ont déjà leurs valeurs enregistrées).
    {
        const bool resync = macroResync_.exchange (false, std::memory_order_acq_rel);
        for (int m = 0; m < neptune::core::control::kMacros; ++m)
        {
            const double v = params_.getNormalizedAt (macroFirst_ + static_cast<std::size_t> (m));
            if (v == macroSeen_[static_cast<std::size_t> (m)]) continue;
            macroSeen_[static_cast<std::size_t> (m)] = v;
            if (! resync) control_.macroChanged (m, v, params_);
        }
    }

    engine_.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    midi.clear();   // Neptune n'a pas de sortie MIDI « de passage » : seuls les messages envoyés à la main partent vers l'hôte (addOutboundEventsToQueue)
}

bool NeptuneProcessor::postMidi (const std::vector<std::uint8_t>& bytes, int targets)
{
    if (bytes.empty() || bytes.size() > 1024 || (targets & 3) == 0) return false;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    midiFifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 < 1) return false;
    auto& p = midiPackets_[static_cast<std::size_t> (size1 > 0 ? start1 : start2)];
    p.size = static_cast<std::uint16_t> (bytes.size());
    p.targets = static_cast<std::uint8_t> (targets & 3);
    std::copy (bytes.begin(), bytes.end(), p.bytes.begin());
    midiFifo_.finishedWrite (1);
    return true;
}

void NeptuneProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const std::string text = currentStateText();
    dest.append (text.data(), text.size());
}

std::string NeptuneProcessor::globalStateText() const
{
    return neptune::core::globalpreset::withoutInstanceData (currentStateText());
}

bool NeptuneProcessor::uiLoadGlobalState (const std::string& stateText, std::string* error)
{
    const auto result = neptune::core::state::load (params_, stateText);
    if (! result.ok())
    {
        if (error != nullptr) *error = result.message;   // refusé en bloc : rien n'a changé
        return false;
    }
    undo_.clear();
    macroResync_.store (true);
    notifyHostOnSeed_ = false;
    adoptMixerFromState (result.mixer);
    notifyHostOnSeed_ = true;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    notifyStateChanged();
    return true;
}

void NeptuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) return;

    const auto result = neptune::core::state::load (
        params_, std::string_view (static_cast<const char*> (data), static_cast<size_t> (sizeInBytes)));
    if (! result.ok())
    {
        // refusé en bloc : aucun paramètre modifié
        DBG ("Neptune: state not loaded: " << juce::String (result.message));
        return;
    }

    undo_.clear(); // l'historique d'un autre état ne s'applique plus (il restaurerait un ancien instrument)
    profile_.setLocked (result.profileLock); // avant adoptStateIdentity : la référence de l'autosave le contient
    // couche de contrôle : absente des projets d'avant (tout désactivé)
    macroResync_.store (true);
    control_.configure (result.controlJson.empty() ? neptune::core::control::Config {} : neptune::core::control::configFromJson (result.controlJson), params_);
    notifyHostOnSeed_ = false; // le rescan ci-dessous prévient l'hôte des valeurs d'enveloppe recopiées depuis les descriptions
    adoptMixerFromState (result.mixer);
    notifyHostOnSeed_ = true;
    adoptStateIdentity (result.instanceId);

    // Les valeurs ont changé hors des événements de paramètres : le wrapper en tire un `params.rescan(VALUES)`
    // vers l'hôte (exigé par le CLAP et vérifié par clap-validator, test state-reproducibility).
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    refreshLatency();   // le verrou de profil et les inserts de l'état chargé fixent la latence à annoncer
}

bool NeptuneProcessor::supportsDirectEvent (uint16_t spaceId, uint16_t type)
{
    if (spaceId != CLAP_CORE_EVENT_SPACE_ID) return false;
    return type == CLAP_EVENT_NOTE_ON || type == CLAP_EVENT_NOTE_OFF || type == CLAP_EVENT_NOTE_CHOKE
        || type == CLAP_EVENT_NOTE_EXPRESSION;
}

// Thread audio, appelé par le wrapper au milieu du bloc, avant processBlock.
void NeptuneProcessor::handleDirectEvent (const clap_event_header_t* event, int)
{
    switch (event->type)
    {
        case CLAP_EVENT_NOTE_ON:
        {
            const auto* n = reinterpret_cast<const clap_event_note*> (event);
            engine_.noteOn (n->channel, n->key, static_cast<float> (n->velocity));
            hostProbe_.lastNoteId = n->note_id;
            hostProbe_.lastKey = n->key;
            ++hostProbe_.clapNoteOns;
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
        {
            const auto* n = reinterpret_cast<const clap_event_note*> (event);
            engine_.noteOff (n->channel, n->key);
            ++hostProbe_.clapNoteOffs;
            // Le moteur ne produit pas encore de son : la note est terminée tout de suite.
            if (numPendingEnds_ < static_cast<int> (pendingEnds_.size()))
                pendingEnds_[static_cast<size_t> (numPendingEnds_++)] = { n->note_id, n->port_index, n->channel, n->key };
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            const auto* e = reinterpret_cast<const clap_event_note_expression*> (event);
            if (e->expression_id >= 0 && e->expression_id < HostProbe::numExpressions)
            {
                const auto i = static_cast<size_t> (e->expression_id);
                hostProbe_.expressionLast[i] = e->value;
                ++hostProbe_.expressionCount[i];
            }
            break;
        }
        default: break;
    }
}

void NeptuneProcessor::addOutboundEventsToQueue (const clap_output_events* out, const juce::MidiBuffer&, int sampleOffset)
{
    // messages MIDI envoyés à la main vers l'hôte
    for (int i = 0; i < numHostMidi_; ++i)
    {
        const auto& p = hostMidi_[static_cast<std::size_t> (i)];
        if (p.bytes[0] == 0xF0)
        {
            clap_event_midi_sysex ev {};
            ev.header.size = sizeof (clap_event_midi_sysex);
            ev.header.time = static_cast<std::uint32_t> (sampleOffset);
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_MIDI_SYSEX;
            ev.header.flags = 0;
            ev.port_index = 0;
            ev.buffer = p.bytes.data();
            ev.size = p.size;
            out->try_push (out, &ev.header);
        }
        else if (p.size >= 1 && p.size <= 3)
        {
            clap_event_midi ev {};
            ev.header.size = sizeof (clap_event_midi);
            ev.header.time = static_cast<std::uint32_t> (sampleOffset);
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_MIDI;
            ev.header.flags = 0;
            ev.port_index = 0;
            for (int b = 0; b < 3; ++b) ev.data[b] = b < p.size ? p.bytes[static_cast<std::size_t> (b)] : 0;
            out->try_push (out, &ev.header);
        }
    }
    numHostMidi_ = 0;

    for (int i = 0; i < numPendingEnds_; ++i)
    {
        const auto& p = pendingEnds_[static_cast<size_t> (i)];
        clap_event_note ev {};
        ev.header.size = sizeof (clap_event_note);
        ev.header.time = static_cast<std::uint32_t> (sampleOffset);
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_NOTE_END;
        ev.header.flags = 0;
        ev.note_id = p.noteId;
        ev.port_index = p.port;
        ev.channel = p.channel;
        ev.key = p.key;
        ev.velocity = 0.0;
        if (out->try_push (out, &ev.header))
            ++hostProbe_.noteEndsSent;
    }
    numPendingEnds_ = 0;
}

juce::AudioProcessorEditor* NeptuneProcessor::createEditor() { return new NeptunePluginEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NeptuneProcessor(); }
