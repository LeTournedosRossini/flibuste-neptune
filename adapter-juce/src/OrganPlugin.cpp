#include "OrganPlugin.h"
#include "MotifLibrary.h"
#include "NovaOrganEditor.h"
#include "NeptuneParameter.h"
#include "neptune/core/Mixer.h"
#include "neptune/core/SamplerParams.h"
#include "neptune/core/ChordTimeline.h"
#include <nlohmann/json.hpp>

using namespace neptune::core;
using neptune::core::theme::Token;

namespace
{
std::filesystem::path configDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return std::filesystem::u8path (base.getChildFile ("Neptune").getFullPathName().toStdString());
}

theme::Palette makePalette (const settings::GlobalSettings& st)
{
    const auto id = theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark);
    return theme::withContrast (theme::definition (id), st.contrast);
}

} // namespace

// ---------------------------------------------------------------------------------------------------- processeur

juce::AudioProcessor::BusesProperties OrganProcessor::makeBuses()
{
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

OrganProcessor::OrganProcessor() : juce::AudioProcessor (makeBuses())
{
    jassert (engine_.ok());
    auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i) addParameter (new NeptuneParameter (set, i, probe_));
    std::vector<std::string> warnings;
    const auto dir = configDirectory();
    settings_ = settings::loadFile (dir / "settings.json", &warnings);
    // la même bibliothèque que Neptune et les autres synthés (dossier choisi dans les réglages, sinon <config>/AnalogSynth)
    std::filesystem::path root = settings_.presetsDir.empty() ? dir / "AnalogSynth" : std::filesystem::u8path (settings_.presetsDir);
    library_ = std::make_unique<preset::Library> (std::move (root), organ::factoryPresets());
    library_->open (&warnings);
    {
        auto cfg = library_->config();
        if (cfg.parameterCaptureDefaults.count (organ::leslieSpeedKey) == 0)
        {
            cfg.parameterCaptureDefaults[organ::leslieSpeedKey] = preset::Capture::Free; // la vitesse du Leslie : libre par défaut
            library_->setConfig (cfg);
        }
    }
    for (auto& s : modal_) s = {};
    carteEnv_ = std::make_unique<arp::ChordMapEnvironment> (reader_, audioMotifs_);   // Carte appelle les motifs par leur numéro : ce sont les emplacements d'Orgamon
    pedalEnv_.base = carteEnv_.get();
    ticker_.startTimer (500);
}

// Fichier libre dans la bibliothèque pour ce motif (« Valse.json », puis « Valse 2.json »...).
static juce::File freeLibraryFile (const neptune::core::pattern::Pattern& pt)
{
    auto f = motifs::fileFor (pt);
    for (int i = 2; f.existsAsFile() && i < 1000; ++i) f = f.getSiblingFile (f.getFileNameWithoutExtension().upToLastOccurrenceOf (" ", false, false).trimEnd() + " " + juce::String (i) + ".json");
    return f;
}

bool OrganProcessor::saveMotifToLibrary (int slot)
{
    if (! motifUsed (slot)) return false;
    neptune::core::pattern::Pattern pt;
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        pt = *motifs_[(size_t) slot];
    }
    auto f = motifs::fileFor (pt);   // même nom : on remplace la version de la bibliothèque
    return f.replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, "orgamon")));
}

void OrganProcessor::importMotifs()
{
    chooser_ = std::make_shared<juce::FileChooser> (juce::String::fromUTF8 ("Importer des motifs (format d'Arpège ou de Scorsaire)"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.json");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        const auto r = neptune::core::pattern::fromJson (f.loadFileAsString().toStdString());
        if (! r.ok) return;
        for (const auto& pt : r.patterns) freeLibraryFile (pt).replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, "orgamon")));
    });
}

void OrganProcessor::exportMotif (int slot)
{
    if (! motifUsed (slot)) return;
    neptune::core::pattern::Pattern pt;
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        pt = *motifs_[(size_t) slot];
    }
    chooser_ = std::make_shared<juce::FileChooser> (juce::String::fromUTF8 ("Exporter le motif"), motifs::fileFor (pt), "*.json");
    chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting, [pt] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
        f.replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, "orgamon")));
    });
}

void OrganProcessor::tick()
{
    reader_.setChannel (carteChannel_.load());
    reader_.poll();
}

juce::String OrganProcessor::carteText() const
{
    if (! carteFollow_.load()) return juce::String::fromUTF8 ("Carte : OFF");
    juce::String s = juce::String::fromUTF8 ("Carte : ON  ·  canal ") + juce::String (carteChannel_.load());
    if (! reader_.connected()) return s + juce::String::fromUTF8 ("  ·  non connectée");
    const int v = carteChordNow_.load();
    if (v < 0) return s + juce::String::fromUTF8 ("  ·  en attente de la lecture");
    chords::Chord c;
    c.root = v & 0xFF;
    c.rel = static_cast<std::uint16_t> (v >> 8 & 0xFFFF);
    return s + "  ·  " + juce::String::fromUTF8 (chords::formatChord (c, chords::Naming::Letters).c_str());
}

bool OrganProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void OrganProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    arp_.setSampleRate (sampleRate);
    arp_.reset();
    pedalArp_.setSampleRate (sampleRate);
    pedalArp_.reset();
}

bool OrganProcessor::motifUsed (int i) const
{
    const juce::SpinLock::ScopedLockType l (motifLock_);
    return i >= 0 && i < kMotifSlots && motifs_[(size_t) i] != nullptr;
}

juce::String OrganProcessor::motifName (int i) const
{
    const juce::SpinLock::ScopedLockType l (motifLock_);
    if (i < 0 || i >= kMotifSlots || motifs_[(size_t) i] == nullptr) return {};
    const auto& p = *motifs_[(size_t) i];
    return juce::String::fromUTF8 ((p.name.empty() ? p.id : p.name).c_str());
}

void OrganProcessor::setMotif (int i, const neptune::core::pattern::Pattern* p)
{
    if (i < 0 || i >= kMotifSlots) return;
    std::shared_ptr<const neptune::core::pattern::Pattern> np;
    if (p != nullptr) np = std::make_shared<const neptune::core::pattern::Pattern> (*p);
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        if (motifs_[(size_t) i] != nullptr) retiredMotifs_.push_back (motifs_[(size_t) i]);   // le fil audio peut encore le lire : on le garde en vie
        if (retiredMotifs_.size() > 64) retiredMotifs_.erase (retiredMotifs_.begin(), retiredMotifs_.begin() + 32);
        motifs_[(size_t) i] = std::move (np);
    }
    motifsVersion_.fetch_add (1);
}

void OrganProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 1) return;
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Carte : pendant la lecture, l'orgue tient l'accord de la carte (notes ajoutées aux événements de l'hôte, à leur instant dans le bloc)
    {
        bool playing = false, hasPos = false;
        double ppq = 0.0, bpm = 120.0;
        if (auto* head = getPlayHead())
            if (const auto info = head->getPosition())
            {
                playing = info->getIsPlaying();
                if (const auto p = info->getPpqPosition()) { hasPos = true; ppq = *p; }
                if (const auto b = info->getBpm()) bpm = *b;
            }
        const bool follow = carteFollow_.load (std::memory_order_relaxed);
        // motif choisi : le moteur d'Arpège le joue d'après la carte ; sinon le suiveur tient l'accord
        {
            const int ver = motifsVersion_.load (std::memory_order_relaxed);
            if (ver != audioMotifsVersion_)
            {
                const juce::SpinLock::ScopedTryLockType l (motifLock_);
                if (l.isLocked()) { audioMotifs_ = motifs_; audioMotifsVersion_ = ver; }
            }
        }
        auto slotMotif = [&] (int slot) -> const pattern::Pattern*
        {
            return (slot >= 1 && slot <= kMotifSlots && audioMotifsVersion_ == motifsVersion_.load (std::memory_order_relaxed)) ? audioMotifs_[(size_t) slot - 1].get() : nullptr;
        };
        const pattern::Pattern* motif = slotMotif (motifSlot_.load (std::memory_order_relaxed));
        const pattern::Pattern* pedalMotif = slotMotif (pedalSlot_.load (std::memory_order_relaxed));
        // le clavier joue des motifs si un motif est choisi ou si Carte en appelle (voie des motifs) ; sinon le suiveur tient l'accord
        const bool usePattern = follow && (motif != nullptr || carteEnv_->hasPatterns());
        const bool usePedal = follow && pedalMotif != nullptr;
        const int chordCh = chordChannel_.load (std::memory_order_relaxed) - 1, chordCh2 = chordChannel2_.load (std::memory_order_relaxed) - 1;
        const int bassCh = bassChannel_.load (std::memory_order_relaxed) - 1;
        const bool led = carteLed_.load (std::memory_order_relaxed);
        const auto put = [&] (bool on, int ch, int note, int vel, int at)
        {
            if (ch < 0) return;
            midi.addEvent (on ? juce::MidiMessage::noteOn (ch + 1, note, static_cast<juce::uint8> (vel)) : juce::MidiMessage::noteOff (ch + 1, note), at);
        };
        follower_.setChordChannel (chordCh);
        follower_.setVoiceLeading (led);
        follower_.setBassChannel (usePedal ? -1 : bassCh);   // un motif de pédale remplace la basse tenue
        follower_.setBaseNote (12 * (carteOctave_.load (std::memory_order_relaxed) + 1));
        const int m = follower_.process (*carteEnv_, follow && ! usePattern && playing, hasPos, ppq, bpm, getSampleRate(), n, followOut_.data(), static_cast<int> (followOut_.size()));
        for (int i = 0; i < m; ++i)
        {
            const auto& e = followOut_[static_cast<size_t> (i)];
            put (e.on, e.channel, e.note, 100, e.offset);
            if (e.channel == chordCh) put (e.on, chordCh2, e.note, 100, e.offset);
        }
        {
            arp::Transport tr;
            tr.hasPosition = hasPos;
            tr.ppq = ppq;
            tr.bpm = bpm;
            if (auto* head = getPlayHead())
                if (const auto info = head->getPosition())
                {
                    if (const auto ts = info->getTimeSignature())
                        if (ts->denominator > 0) tr.beatsPerBar = 4.0 * ts->numerator / ts->denominator;
                    if (const auto bar = info->getPpqPositionOfLastBarStart()) tr.barStartPpq = *bar;
                }
            arp::Params ap;
            ap.gate = 0.9;
            ap.fixedVelocity = true;
            ap.velocity = 100;
            arp::ApproachParams approach;
            approach.kind = static_cast<arp::ApproachParams::Kind> (carteApproach_.load (std::memory_order_relaxed));
            approach.chance = approachChance_.load (std::memory_order_relaxed) / 100.0;
            approach.below = approachBelow_.load (std::memory_order_relaxed) / 100.0;
            const int baseNote = 12 * (carteOctave_.load (std::memory_order_relaxed) + 1);
            // clavier : le motif choisi (ou celui que Carte appelle) ; les parties B vont à la pédale, sauf si la pédale a son propre motif
            arp_.setParams (ap);
            arp_.setPattern (usePattern ? motif : nullptr);
            arp_.setEnvironment (usePattern ? carteEnv_.get() : nullptr);
            arp_.setBaseNote (baseNote);
            arp_.setVoiceLeading (led);
            arp_.setApproach (approach);
            tr.playing = usePattern && playing;
            const int k = arp_.process (nullptr, 0, tr, n, arpOut_.data(), static_cast<int> (arpOut_.size()));
            for (int i = 0; i < k; ++i)
            {
                const auto& e = arpOut_[static_cast<size_t> (i)];
                if (e.bass)
                {
                    if (! usePedal) put (e.on, bassCh, e.note, e.velocity, e.offset);   // pas de basse ou motif de pédale : la partie B se tait (début et fin)
                }
                else
                {
                    put (e.on, chordCh, e.note, e.velocity, e.offset);
                    put (e.on, chordCh2, e.note, e.velocity, e.offset);
                }
            }
            // pédale : son motif, toutes ses notes sur le canal de la pédale
            pedalArp_.setParams (ap);
            pedalArp_.setPattern (usePedal ? pedalMotif : nullptr);
            pedalArp_.setEnvironment (usePedal ? &pedalEnv_ : nullptr);
            pedalArp_.setBaseNote (baseNote - 12);
            pedalArp_.setVoiceLeading (led);
            pedalArp_.setApproach (approach);
            tr.playing = usePedal && playing;
            const int kp = pedalArp_.process (nullptr, 0, tr, n, pedalOut_.data(), static_cast<int> (pedalOut_.size()));
            for (int i = 0; i < kp; ++i)
            {
                const auto& e = pedalOut_[static_cast<size_t> (i)];
                put (e.on, bassCh, e.note, e.velocity, e.offset);
            }
        }
        arp::Harmony hm;
        if (follow && playing && hasPos && reader_.connected() && carteEnv_->harmonyAt (ppq, hm)) carteChordNow_.store (hm.rootPc | static_cast<int> (hm.rel) << 8);
        else carteChordNow_.store (-2);
    }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (pos, n, meta.samplePosition);
        if (at > pos)
        {
            engine_.process (left + pos, right != nullptr ? right + pos : nullptr, at - pos);
            pos = at;
        }
        const auto m = meta.getMessage();
        if (m.isNoteOn())
        {
            const int ch = (m.getChannel() - 1) & 15;
            ++heldPerChannel_[ch];
            midiLevel_[ch].store (m.getFloatVelocity(), std::memory_order_relaxed);
            engine_.noteOn (m.getChannel() - 1, m.getNoteNumber());
        }
        else if (m.isNoteOff())
        {
            const int ch = (m.getChannel() - 1) & 15;
            if (heldPerChannel_[ch] > 0 && --heldPerChannel_[ch] == 0) midiLevel_[ch].store (0.0f, std::memory_order_relaxed);
            engine_.noteOff (m.getChannel() - 1, m.getNoteNumber());
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (int c = 0; c < 16; ++c) { heldPerChannel_[c] = 0; midiLevel_[c].store (0.0f, std::memory_order_relaxed); }
            if (m.isAllNotesOff()) engine_.allNotesOff(); else engine_.panic();
        }
        else if (m.isController() && m.getControllerNumber() == 0) midiBank_.store (juce::jlimit (0, 127, m.getControllerValue()));
        else if (m.isController() && ccEnabled_.load (std::memory_order_relaxed)) engine_.controlChange (m.getChannel() - 1, m.getControllerNumber(), m.getControllerValue());
        else if (m.isProgramChange())
        {
            pendingBank_.store (midiBank_.load());
            pendingProgram_.store (m.getProgramChangeNumber());
            triggerAsyncUpdate();
        }
    }
    if (pos < n) engine_.process (left + pos, right != nullptr ? right + pos : nullptr, n - pos);
    {
        float pl = 0.0f, pr = 0.0f;
        for (int i = 0; i < n; ++i) { pl = std::max (pl, std::fabs (left[i])); pr = std::max (pr, std::fabs (right != nullptr ? right[i] : left[i])); }
        outPeak_[0].store (pl, std::memory_order_relaxed);
        outPeak_[1].store (pr, std::memory_order_relaxed);
    }
    midi.clear();
}

void OrganProcessor::handleAsyncUpdate()
{
    const int bank = pendingBank_.exchange (-1), program = pendingProgram_.exchange (-1);
    if (bank < 0 || program < 0) return;
    const auto id = preset::Navigator::presetAt (library_->banks(), { bank, program });
    if (! id.empty()) loadPreset (id);
}

void OrganProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto text = organ::stateToJson (engine_.params(), engine_.description(), currentPresetId_, ccEnabled_.load());
    // les réglages de Carte ne sont pas des paramètres : une clé de plus dans le document d'état (les anciens états n'en ont pas)
    auto doc = nlohmann::json::parse (text, nullptr, false);
    if (doc.is_object())
    {
        doc["carte"] = { { "follow", carteFollow_.load() }, { "channel", carteChannel_.load() }, { "chordChannel", chordChannel_.load() }, { "bassChannel", bassChannel_.load() }, { "octave", carteOctave_.load() }, { "led", carteLed_.load() }, { "motifSlot", motifSlot_.load() }, { "approach", carteApproach_.load() }, { "chordChannel2", chordChannel2_.load() }, { "pedalSlot", pedalSlot_.load() }, { "approachChance", approachChance_.load() }, { "approachBelow", approachBelow_.load() } };
        auto arr = nlohmann::json::array();
        {
            const juce::SpinLock::ScopedLockType l (motifLock_);
            for (const auto& mp : motifs_) arr.push_back (mp != nullptr ? neptune::core::pattern::toJson ({ *mp }, "orgamon") : std::string());
        }
        doc["carte"]["motifs"] = arr;
        text = doc.dump();
    }
    dest.append (text.data(), text.size());
}

void OrganProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    organ::OrganDescription d;
    std::string presetId;
    const std::string text (static_cast<const char*> (data), static_cast<std::size_t> (size));
    bool cc = false;
    if (! organ::stateFromJson (text, engine_.params(), d, presetId, &cc)) return;
    ccEnabled_.store (cc);
    {
        const auto doc = nlohmann::json::parse (text, nullptr, false);
        if (doc.is_object() && doc.contains ("carte") && doc["carte"].is_object())
        {
            const auto& c = doc["carte"];
            carteFollow_.store (c.value ("follow", false));
            carteChannel_.store (juce::jlimit (1, 8, c.value ("channel", 1)));
            chordChannel_.store (juce::jlimit (1, 16, c.value ("chordChannel", 1)));
            bassChannel_.store (juce::jlimit (0, 16, c.value ("bassChannel", 3)));
            carteOctave_.store (juce::jlimit (1, 6, c.value ("octave", 3)));
            carteLed_.store (c.value ("led", true));
            motifSlot_.store (juce::jlimit (0, kMotifSlots, c.value ("motifSlot", 0)));
            carteApproach_.store (juce::jlimit (0, 4, c.value ("approach", 0)));
            chordChannel2_.store (juce::jlimit (0, 16, c.value ("chordChannel2", 0)));
            pedalSlot_.store (juce::jlimit (0, kMotifSlots, c.value ("pedalSlot", 0)));
            approachChance_.store (juce::jlimit (0, 100, c.value ("approachChance", 100)));
            approachBelow_.store (juce::jlimit (0, 100, c.value ("approachBelow", 0)));
            for (int i = 0; i < kMotifSlots; ++i)
            {
                neptune::core::pattern::Pattern pt;
                bool ok = false;
                if (c.contains ("motifs") && c["motifs"].is_array() && (size_t) i < c["motifs"].size() && c["motifs"][(size_t) i].is_string())
                {
                    const auto r = neptune::core::pattern::fromJson (c["motifs"][(size_t) i].get<std::string>());
                    if (r.ok && ! r.patterns.empty()) { pt = r.patterns.front(); ok = true; }
                }
                setMotif (i, ok ? &pt : nullptr);
            }
        }
    }
    engine_.setDescription (d);
    currentPresetId_ = presetId;
    undo_.clear();
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

void OrganProcessor::uiBeginEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    getParameters()[static_cast<int> (index)]->beginChangeGesture();
    undo_.beginGesture();
}

void OrganProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = engine_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    getParameters()[static_cast<int> (index)]->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void OrganProcessor::uiEndEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    getParameters()[static_cast<int> (index)]->endChangeGesture();
}

void OrganProcessor::uiResetToDefault (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = engine_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool OrganProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

bool OrganProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

void OrganProcessor::uiEditOrgan (organ::OrganDescription d, std::string name, std::string mergeKey)
{
    d = organ::sanitised (std::move (d));
    const auto previous = engine_.description();
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<organ::OrganDescription>> (
        previous, d,
        [this] (const organ::OrganDescription& v)
        {
            engine_.setDescription (v);
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
        },
        std::move (name), std::move (mergeKey)));
}

// ---- presets
std::vector<float> OrganProcessor::snapshotNormalised() const
{
    std::vector<float> v;
    const auto& set = engine_.params();
    v.reserve (set.size());
    for (std::size_t i = 0; i < set.size(); ++i) v.push_back (static_cast<float> (set.getNormalizedAt (i)));
    return v;
}

void OrganProcessor::notifyHostOfParameterChanges (const std::vector<float>& before)
{
    const auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size() && i < before.size(); ++i)
    {
        const auto now = static_cast<float> (set.getNormalizedAt (i));
        if (now == before[i]) continue;
        auto* p = getParameters()[static_cast<int> (i)];
        p->beginChangeGesture();
        p->setValueNotifyingHost (now);
        p->endChangeGesture();
    }
}

bool OrganProcessor::loadPreset (const std::string& id)
{
    const auto* p = library_->find (id);
    if (p == nullptr || ! acceptsPreset (*p)) return false;
    const auto before = snapshotNormalised();
    organ::applyPresetValues (*p, engine_.params(), library_->config());
    currentPresetId_ = id;
    undo_.clear();
    notifyHostOfParameterChanges (before);
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return true;
}

bool OrganProcessor::stepPreset (int direction)
{
    const auto next = library_->stepFrom (currentPresetId_, direction, [this] (const preset::Preset& p) { return acceptsPreset (p); });
    return ! next.empty() && loadPreset (next);
}

bool OrganProcessor::isModified() const
{
    const auto* p = currentPreset();
    return p != nullptr && organ::differsFromPreset (*p, engine_.params(), library_->config());
}

bool OrganProcessor::canSaveCurrent() const
{
    const auto* p = currentPreset();
    return p != nullptr && ! p->factory && ! p->locked;
}

bool OrganProcessor::overwritePreset (const std::string& id, std::string* error)
{
    const auto* base = library_->find (id);
    if (base == nullptr || base->factory) { if (error) *error = "factory preset is read-only"; return false; }
    if (base->locked) { if (error) *error = "preset is locked"; return false; }
    auto p = organ::capturePreset (engine_.params(), library_->config(), base->name, base->type, base);
    const bool ok = library_->save (std::move (p), error);
    if (ok) ++presetRevision_;
    return ok;
}

bool OrganProcessor::saveCurrent (std::string* error)
{
    if (currentPresetId_.empty()) { if (error) *error = "no current preset"; return false; }
    return overwritePreset (currentPresetId_, error);
}

bool OrganProcessor::saveCurrentAs (const std::string& name, const std::string& type, preset::Preset* out, std::string* error)
{
    auto p = organ::capturePreset (engine_.params(), library_->config(), name, type);
    if (const auto* cur = currentPreset())
    {
        p.characteristics = cur->characteristics;
        p.customFields = cur->customFields;
        p.rating = cur->rating;
        p.captureOverrides = cur->captureOverrides;
        if (p.type.empty()) p.type = cur->type;
        p.derivedFrom = cur->factory ? cur->id : cur->derivedFrom;
        p = organ::capturePreset (engine_.params(), library_->config(), name, p.type, &p);
    }
    preset::Preset stored;
    if (! library_->importPreset (std::move (p), &stored, error)) return false;
    currentPresetId_ = stored.id;
    ++presetRevision_;
    if (out != nullptr) *out = stored;
    return true;
}

bool OrganProcessor::saveConfig (const preset::LibraryConfig& c)
{
    const bool ok = library_->setConfig (c);
    if (ok) ++presetRevision_;
    return ok;
}

juce::AudioProcessorEditor* OrganProcessor::createEditor() { return new NovaOrganPluginEditor (*this); }

void OrganProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ------------------------------------------------------------------------------------------------ fenêtre du plugin
NovaOrganPluginEditor::NovaOrganPluginEditor (OrganProcessor& p) : juce::AudioProcessorEditor (p), processor_ (p), tr_ (p.settings().language)
{
    buildContent();
    setWantsKeyboardFocus (true);
    nova::configureStretchConstrainer (constrainer_, nova::EditorBase::designWidth, nova::EditorBase::designHeight, nova::EditorBase::minScale, nova::EditorBase::maxScale, 4096);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (nova::EditorBase::designWidth, nova::EditorBase::designHeight);
}

void NovaOrganPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto id = theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark);
    const auto palette = theme::withContrast (theme::definition (id), p.settings().contrast);
    const auto& def = theme::definition (id);
    NovaOrganEditor::Options opt;
    opt.brand = true;
    opt.stretchAuto = true;   // fenêtre étirable en largeur
    opt.noBypass = true;
    opt.moduleName = "organ";
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    opt.applySettings = [&p, safe = juce::Component::SafePointer<NovaOrganPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    opt.presetMenu = [this] (juce::Component* anchor) { if (content_ != nullptr) content_->showPresetMenu (anchor); };
    content_.reset();
    OrganLink link;
    link.description = [&p] { return p.organDescription(); };
    link.edit = [&p] (organ::OrganDescription d) { p.uiEditOrgan (std::move (d), "Organ keyboards", "organ"); };
    link.undoRevision = [&p] { return static_cast<std::uint32_t> (p.undoManager().revision()); };
    link.panic = [&p] { p.uiPanic(); };
    link.noteOn = [&p] (int ch, int key) { p.uiNoteOn (ch, key); };
    link.noteOff = [&p] (int ch, int key) { p.uiNoteOff (ch, key); };
    link.heldWord = [&p] (int word) { return p.engine().heldKeyWord (word); };
    link.heldCount = [&p] { return p.engine().heldKeys(); };
    link.outPeak = [&p] (int c) { return p.outPeak (c); };
    link.midiLevel = [&p] (int ch) { return p.midiLevel (ch); };
    link.ccEnabled = [&p] { return p.ccEnabled(); };
    link.setCc = [&p] (bool on) { p.uiSetCcEnabled (on); };
    link.carteText = [&p] { return p.carteText(); };
    link.carteMenu = [&p] (juce::Component* anchor)
    {
        const auto U = [] (const char* t) { return juce::String::fromUTF8 (t); };
        auto* look = &anchor->getLookAndFeel();   // le style de la suite pour le menu et ses sous-menus (sinon celui de JUCE)
        const auto sub = [look] { juce::PopupMenu mm; mm.setLookAndFeel (look); return mm; };
        const auto library = motifs::files();   // lu à l'ouverture du menu
        const auto slotLabel = [&] (int i) { return juce::String (i + 1) + " : " + (p.motifUsed (i) ? p.motifName (i) : U ("(vide)")); };
        auto m = sub();
        m.addItem (1, U ("Suivre les accords de Carte"), true, p.carteFollow());
        m.addItem (2, U ("Voix conduites (sinon position fixe)"), true, p.carteLed());
        m.addSeparator();

        // clavier : upper, lower ou les deux (les accords tenus comme les motifs)
        auto kb = sub(), other = sub();
        const int c1 = p.chordMidiChannel(), c2 = p.chordMidiChannel2();
        kb.addItem (201, U ("Upper (canal 1)"), true, c1 == 1 && c2 == 0);
        kb.addItem (202, U ("Lower (canal 2)"), true, c1 == 2 && c2 == 0);
        kb.addItem (203, U ("Upper et lower"), true, c1 == 1 && c2 == 2);
        for (int i = 1; i <= 16; ++i) other.addItem (220 + i, juce::String (i), true, c2 == 0 && c1 == i);
        kb.addSubMenu (U ("Autre canal"), other);
        m.addSubMenu (U ("Clavier des accords et des motifs"), kb);

        auto pedal = sub();
        pedal.addItem (300, U ("aucune pédale"), true, p.bassMidiChannel() == 0);
        for (int i = 1; i <= 16; ++i) pedal.addItem (300 + i, i == 3 ? U ("3 (pédale)") : juce::String (i), true, p.bassMidiChannel() == i);
        m.addSubMenu (U ("Canal MIDI de la pédale"), pedal);
        m.addSeparator();

        auto play = sub(), playPedal = sub();
        play.addItem (500, U ("aucun (tenir l'accord)"), true, p.motifSlot() == 0);
        playPedal.addItem (700, U ("aucun (la partie B du motif du clavier)"), true, p.pedalMotifSlot() == 0);
        for (int i = 0; i < OrganProcessor::kMotifSlots; ++i)
        {
            play.addItem (501 + i, slotLabel (i), p.motifUsed (i), p.motifSlot() == i + 1);
            playPedal.addItem (701 + i, slotLabel (i), p.motifUsed (i), p.pedalMotifSlot() == i + 1);
        }
        m.addSubMenu (U ("Motif du clavier"), play);
        m.addSubMenu (U ("Motif de la pédale"), playPedal);

        auto appr = sub();
        {
            const char* names[] = { "aucun", "dominante (V7 de l'accord suivant)", "triton (bII7)", "chromatique (majeur un demi-ton plus bas)", "parallèle (l'accord suivant, un demi-ton plus haut)" };
            for (int k = 0; k < 5; ++k) appr.addItem (600 + k, U (names[k]), true, p.carteApproach() == k);
            appr.addSeparator();
            const int pcts[] = { 25, 50, 75, 100 };
            for (int k = 0; k < 4; ++k) appr.addItem (810 + k, juce::String (pcts[k]) + U (" % des changements"), p.carteApproach() != 0, p.approachChance() == pcts[k]);
            appr.addSeparator();
            appr.addItem (820, U ("parallèle : toujours au-dessus"), p.carteApproach() == 4, p.approachBelow() == 0);
            appr.addItem (821, U ("parallèle : toujours en dessous"), p.carteApproach() == 4, p.approachBelow() == 100);
            appr.addItem (822, U ("parallèle : au-dessus et en dessous"), p.carteApproach() == 4, p.approachBelow() == 50);
        }
        m.addSubMenu (U ("Accord d'approche"), appr);
        m.addSeparator();

        // bibliothèque : charger, enregistrer, exporter, importer
        auto lib = sub(), load = sub(), save = sub(), exp = sub();
        for (int i = 0; i < OrganProcessor::kMotifSlots; ++i)
        {
            auto files = sub();
            files.addItem (10000 + i * 256, U ("vider l'emplacement"), p.motifUsed (i));
            for (int f = 0; f < library.size() && f < 200; ++f) files.addItem (10000 + i * 256 + 1 + f, library[f].getFileNameWithoutExtension());
            if (library.isEmpty()) files.addItem (-1, U ("(la bibliothèque est vide)"), false);
            load.addSubMenu (slotLabel (i), files);
            save.addItem (9100 + i, slotLabel (i), p.motifUsed (i));
            exp.addItem (9200 + i, slotLabel (i), p.motifUsed (i));
        }
        lib.addSubMenu (U ("Charger dans l'emplacement"), load);
        lib.addSubMenu (U ("Enregistrer l'emplacement dans la bibliothèque"), save);
        lib.addSubMenu (U ("Exporter l'emplacement vers un fichier"), exp);
        lib.addItem (9001, U ("Importer un fichier de motifs..."));
        lib.addSeparator();
        lib.addItem (9000, U ("Ouvrir le dossier de la bibliothèque"));
        m.addSubMenu (U ("Bibliothèque de motifs"), lib);
        m.addSeparator();

        auto ch = sub(), oct = sub();
        for (int i = 1; i <= 8; ++i) ch.addItem (100 + i, juce::String (i), true, p.carteChannel() == i);
        for (int i = 1; i <= 6; ++i) oct.addItem (400 + i, juce::String (i) + (i == 3 ? U (" (do 48)") : juce::String()), true, p.carteOctave() == i);
        m.addSubMenu (U ("Canal de Carte"), ch);
        m.addSubMenu (U ("Octave de base"), oct);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [&p] (int r)
        {
            if (r == 1) p.setCarteFollow (! p.carteFollow());
            else if (r == 2) p.setCarteLed (! p.carteLed());
            else if (r > 100 && r <= 108) p.setCarteChannel (r - 100);
            else if (r == 201) { p.setChordMidiChannel (1); p.setChordMidiChannel2 (0); }
            else if (r == 202) { p.setChordMidiChannel (2); p.setChordMidiChannel2 (0); }
            else if (r == 203) { p.setChordMidiChannel (1); p.setChordMidiChannel2 (2); }
            else if (r > 220 && r <= 236) { p.setChordMidiChannel (r - 220); p.setChordMidiChannel2 (0); }
            else if (r >= 300 && r <= 316) p.setBassMidiChannel (r - 300);
            else if (r > 400 && r <= 406) p.setCarteOctave (r - 400);
            else if (r >= 500 && r <= 516) p.setMotifSlot (r - 500);
            else if (r >= 600 && r <= 604) p.setCarteApproach (r - 600);
            else if (r >= 700 && r <= 716) p.setPedalMotifSlot (r - 700);
            else if (r >= 810 && r <= 813) { const int pcts[] = { 25, 50, 75, 100 }; p.setApproachChance (pcts[r - 810]); }
            else if (r == 820) p.setApproachBelow (0);
            else if (r == 821) p.setApproachBelow (100);
            else if (r == 822) p.setApproachBelow (50);
            else if (r == 9000) motifs::directory().revealToUser();
            else if (r == 9001) p.importMotifs();
            else if (r >= 9100 && r < 9116) p.saveMotifToLibrary (r - 9100);
            else if (r >= 9200 && r < 9216) p.exportMotif (r - 9200);
            else if (r >= 10000 && r < 10000 + 16 * 256)
            {
                const int slot = (r - 10000) / 256, idx = (r - 10000) % 256;
                if (idx == 0)
                {
                    p.setMotif (slot, nullptr);
                    if (p.motifSlot() == slot + 1) p.setMotifSlot (0);
                    if (p.pedalMotifSlot() == slot + 1) p.setPedalMotifSlot (0);
                }
                else
                {
                    const auto files = motifs::files();
                    neptune::core::pattern::Pattern pt;
                    if (idx - 1 < files.size() && motifs::read (files[idx - 1], pt)) p.setMotif (slot, &pt);
                }
            }
        });
    };
    link.collect = [&p] { p.engine().collect(); };
    link.track = 0;
    link.volumeId = organ::synthVolume;
    link.gainId = static_cast<ParamId> (sampler::gain);
    content_ = std::make_unique<NovaOrganEditor> (p, p, std::move (link), palette, def.knob, tr_, opt);
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void NovaOrganPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void NovaOrganPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
bool NovaOrganPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OrganProcessor(); }
