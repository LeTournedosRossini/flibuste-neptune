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
}

bool OrganProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void OrganProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    carte_.prepare (sampleRate);
}

void OrganProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 1) return;
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Carte : pendant la lecture, l'orgue joue d'après la carte (notes ajoutées aux événements de l'hôte, à leur instant dans le bloc)
    carte_.process (getPlayHead(), getSampleRate(), n, midi);

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
        carte_.saveState (doc);
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
        carte_.loadState (doc);
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
    link.carteText = [&p] { return p.carte().text(); };
    link.carteMenu = [&p] (juce::Component* anchor) { p.carte().showMenu (anchor); };
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
