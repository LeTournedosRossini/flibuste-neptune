#include "FmPlugin.h"
#include "NovaFmEditor.h"
#include "NeptuneParameter.h"
#include "neptune/core/SamplerParams.h"

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

} // namespace

// ---------------------------------------------------------------------------------------------------- processeur

juce::AudioProcessor::BusesProperties FmSynthProcessor::makeBuses()
{
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

FmSynthProcessor::FmSynthProcessor() : juce::AudioProcessor (makeBuses())
{
    jassert (engine_.ok());
    auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i) addParameter (new NeptuneParameter (set, i, probe_));

    std::vector<std::string> warnings;
    const auto dir = configDirectory();
    settings_ = settings::loadFile (dir / "settings.json", &warnings);
    std::filesystem::path root = settings_.presetsDir.empty() ? dir / "AnalogSynth" : std::filesystem::u8path (settings_.presetsDir);
    library_ = std::make_unique<preset::Library> (std::move (root), fm::factoryPresets());
    library_->open (&warnings);
    for (auto& s : modal_) s = {};
}

bool FmSynthProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void FmSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    rig_.prepare (sampleRate);
    rig_.reset (engine_.params());
}

void FmSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 1) return;
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Molettes de l'écran : une position qui change est transmise comme un message MIDI (molettes du moteur, source de la matrice)
    {
        const float bend = uiBend_.load (std::memory_order_relaxed), mod = uiMod_.load (std::memory_order_relaxed);
        if (bend != lastUiBend_)
        {
            lastUiBend_ = bend;
            engine_.setPitchBend (juce::jlimit (0, 16383, static_cast<int> (std::lround (8192.0f + bend * 8191.0f))));
            rig_.midiEvent ({ midi::MsgKind::PitchBend, 0, 0, juce::jlimit (0, 127, static_cast<int> (std::lround (64.0f + bend * 63.5f))) });
        }
        if (mod != lastUiMod_)
        {
            lastUiMod_ = mod;
            const int v = juce::jlimit (0, 127, static_cast<int> (std::lround (mod * 127.0f)));
            engine_.setModWheel (v);
            rig_.midiEvent ({ midi::MsgKind::ControlChange, 0, 1, v });
        }
    }

    // Matrice de modulation : évaluée une fois par bloc, avant le rendu
    rig_.process (n, engine_.params());

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
        const int channel = juce::jlimit (0, 15, m.getChannel() - 1);
        if (m.isNoteOn()) { engine_.noteOn (m.getChannel() - 1, m.getNoteNumber(), m.getFloatVelocity()); rig_.noteOn (m.getNoteNumber()); }
        else if (m.isNoteOff()) { engine_.noteOff (m.getChannel() - 1, m.getNoteNumber()); rig_.noteOff (m.getNoteNumber()); }
        else if (m.isAllNotesOff()) { engine_.releaseAll(); rig_.releaseAll(); }
        else if (m.isAllSoundOff()) engine_.panic();
        else if (m.isPitchWheel())
        {
            engine_.setPitchBend (juce::jlimit (0, 16383, m.getPitchWheelValue()));
            rig_.midiEvent ({ midi::MsgKind::PitchBend, channel, 0, m.getPitchWheelValue() >> 7 });
        }
        else if (m.isChannelPressure()) rig_.midiEvent ({ midi::MsgKind::ChannelPressure, channel, 0, m.getChannelPressureValue() });
        else if (m.isAftertouch()) rig_.midiEvent ({ midi::MsgKind::PolyPressure, channel, m.getNoteNumber(), m.getAfterTouchValue() });
        else if (m.isController())
        {
            const int cc = m.getControllerNumber(), v = juce::jlimit (0, 127, m.getControllerValue());
            rig_.midiEvent ({ midi::MsgKind::ControlChange, channel, cc, v });
            if (cc == 0) midiBank_.store (v);
            else if (cc == 1) engine_.setModWheel (v);
            else if (cc == 64) engine_.setSustain (v >= 64);
        }
        else if (m.isProgramChange())
        {
            pendingBank_.store (midiBank_.load());
            pendingProgram_.store (m.getProgramChangeNumber());
            triggerAsyncUpdate();
        }
    }
    if (pos < n) engine_.process (left + pos, right != nullptr ? right + pos : nullptr, n - pos);
    midi.clear();
}

void FmSynthProcessor::handleAsyncUpdate()
{
    const int bank = pendingBank_.exchange (-1), program = pendingProgram_.exchange (-1);
    if (bank < 0 || program < 0) return;
    const auto id = preset::Navigator::presetAt (library_->banks(), { bank, program });
    if (! id.empty()) loadPreset (id);
}

void FmSynthProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const auto matrix = rig_.matrix();
    const auto text = fm::stateToJson (engine_.params(), engine_.description(), currentPresetId_, &matrix);
    dest.append (text.data(), text.size());
}

void FmSynthProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    fm::FmDescription d;
    std::string presetId;
    modulation::Matrix matrix;
    const std::string text (static_cast<const char*> (data), static_cast<std::size_t> (size));
    if (! fm::stateFromJson (text, engine_.params(), d, presetId, &matrix)) return;
    engine_.setDescription (d);
    rig_.setMatrix (matrix, engine_.params());
    currentPresetId_ = presetId;
    undo_.clear();
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

void FmSynthProcessor::uiBeginEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    getParameters()[static_cast<int> (index)]->beginChangeGesture();
    undo_.beginGesture();
}

void FmSynthProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = engine_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    getParameters()[static_cast<int> (index)]->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void FmSynthProcessor::uiEndEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    getParameters()[static_cast<int> (index)]->endChangeGesture();
}

void FmSynthProcessor::uiResetToDefault (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = engine_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool FmSynthProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

bool FmSynthProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

void FmSynthProcessor::uiEditFm (fm::FmDescription d, std::string name, std::string mergeKey)
{
    d = fm::sanitised (std::move (d));
    const auto previous = engine_.description();
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<fm::FmDescription>> (
        previous, d,
        [this] (const fm::FmDescription& v)
        {
            const auto before = snapshotNormalised();
            engine_.setDescription (v); // recopie aussi LFO, bend, molette et glissando dans les paramètres
            notifyHostOfParameterChanges (before);
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
        },
        std::move (name), std::move (mergeKey)));
}

// ---- matrice de modulation (onglet MOD)
void FmSynthProcessor::modEdit (int, modulation::Matrix matrix)
{
    matrix = modulation::sanitised (std::move (matrix));
    const auto previous = rig_.matrix();
    if (previous == matrix) return;
    undo_.perform (std::make_unique<SnapshotCommand<modulation::Matrix>> (
        previous, matrix,
        [this] (const modulation::Matrix& v)
        {
            rig_.setMatrix (v, engine_.params());
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
        },
        "Modulation", "modulation"));
}

// La matrice pour un preset : vide si elle l'est (un preset sans matrice reste identique).
std::string FmSynthProcessor::modulationText() const
{
    const auto& m = rig_.matrix();
    return m == modulation::Matrix {} ? std::string() : modulation::toJson (m);
}

std::string FmSynthProcessor::modDestName (int, ParamId dest) const
{
    const auto& set = engine_.params();
    const auto index = set.indexOf (dest);
    return index == ParameterSet::npos ? std::string() : set.spec (index).name;
}

// ---- presets
std::vector<float> FmSynthProcessor::snapshotNormalised() const
{
    std::vector<float> v;
    const auto& set = engine_.params();
    v.reserve (set.size());
    for (std::size_t i = 0; i < set.size(); ++i) v.push_back (static_cast<float> (set.getNormalizedAt (i)));
    return v;
}

void FmSynthProcessor::notifyHostOfParameterChanges (const std::vector<float>& before)
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

bool FmSynthProcessor::loadPreset (const std::string& id)
{
    const auto* p = library_->find (id);
    if (p == nullptr || ! acceptsPreset (*p)) return false;
    const auto before = snapshotNormalised();
    const auto& cfg = library_->config();
    engine_.setDescription (fm::presetDescription (*p, engine_.description(), cfg));
    fm::applyPresetValues (*p, engine_.params(), cfg);
    rig_.setMatrix (p->modulation.empty() ? modulation::Matrix {} : modulation::fromJson (p->modulation), engine_.params());   // un preset sans matrice la vide
    currentPresetId_ = id;
    undo_.clear();
    notifyHostOfParameterChanges (before);
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return true;
}

bool FmSynthProcessor::stepPreset (int direction)
{
    const auto next = library_->stepFrom (currentPresetId_, direction, [this] (const preset::Preset& p) { return acceptsPreset (p); });
    return ! next.empty() && loadPreset (next);
}

bool FmSynthProcessor::isModified() const
{
    const auto* p = currentPreset();
    if (p == nullptr) return false;
    const auto stored = p->modulation.empty() ? modulation::Matrix {} : modulation::sanitised (modulation::fromJson (p->modulation));
    return fm::differsFromPreset (*p, engine_.params(), engine_.description(), library_->config()) || stored != rig_.matrix();
}

bool FmSynthProcessor::canSaveCurrent() const
{
    const auto* p = currentPreset();
    return p != nullptr && ! p->factory && ! p->locked;
}

bool FmSynthProcessor::overwritePreset (const std::string& id, std::string* error)
{
    const auto* base = library_->find (id);
    if (base == nullptr || base->factory) { if (error) *error = "factory preset is read-only"; return false; }
    if (base->locked) { if (error) *error = "preset is locked"; return false; }
    auto p = fm::capturePreset (engine_.params(), engine_.description(), library_->config(), base->name, base->type, base);
    p.modulation = modulationText();
    const bool ok = library_->save (std::move (p), error);
    if (ok) ++presetRevision_;
    return ok;
}

bool FmSynthProcessor::saveCurrent (std::string* error)
{
    if (currentPresetId_.empty()) { if (error) *error = "no current preset"; return false; }
    return overwritePreset (currentPresetId_, error);
}

bool FmSynthProcessor::saveCurrentAs (const std::string& name, const std::string& type, preset::Preset* out, std::string* error)
{
    auto p = fm::capturePreset (engine_.params(), engine_.description(), library_->config(), name, type);
    if (const auto* cur = currentPreset())
    {
        p.characteristics = cur->characteristics;
        p.customFields = cur->customFields;
        p.rating = cur->rating;
        p.captureOverrides = cur->captureOverrides;
        if (p.type.empty()) p.type = cur->type;
        p.derivedFrom = cur->factory ? cur->id : cur->derivedFrom;
        p = fm::capturePreset (engine_.params(), engine_.description(), library_->config(), name, p.type, &p);
    }
    p.modulation = modulationText();
    preset::Preset stored;
    if (! library_->importPreset (std::move (p), &stored, error)) return false;
    currentPresetId_ = stored.id;
    ++presetRevision_;
    if (out != nullptr) *out = stored;
    return true;
}

bool FmSynthProcessor::saveConfig (const preset::LibraryConfig& c)
{
    const bool ok = library_->setConfig (c);
    if (ok) ++presetRevision_;
    return ok;
}

juce::AudioProcessorEditor* FmSynthProcessor::createEditor() { return new NovaFmPluginEditor (*this); }

void FmSynthProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ---------------------------------------------------------------------------------------------------- fenêtre du plugin
NovaFmPluginEditor::NovaFmPluginEditor (FmSynthProcessor& p) : juce::AudioProcessorEditor (p), processor_ (p), tr_ (p.settings().language)
{
    buildContent();
    setWantsKeyboardFocus (true);
    nova::configureConstrainer (constrainer_, nova::EditorBase::designWidth, nova::EditorBase::designHeight, nova::EditorBase::minScale, nova::EditorBase::maxScale);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (nova::EditorBase::designWidth, nova::EditorBase::designHeight);
}

void NovaFmPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto id = theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark);
    const auto palette = theme::withContrast (theme::definition (id), p.settings().contrast);
    const auto& def = theme::definition (id);
    NovaFmEditor::Options opt;
    opt.brand = true;
    opt.stretchAuto = true;   // fenêtre étirable en largeur
    opt.noBypass = true;
    opt.moduleName = "fm6op";
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    opt.applySettings = [&p, safe = juce::Component::SafePointer<NovaFmPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    opt.presetMenu = [this] (juce::Component* anchor) { if (content_ != nullptr) content_->showPresetMenu (anchor); };
    FmLink link;
    link.description = [&p] { return p.fmDescription(); };
    link.edit = [&p] (fm::FmDescription d, std::string name, std::string mergeKey) { p.uiEditFm (std::move (d), std::move (name), std::move (mergeKey)); };
    link.beginGesture = [&p] { p.uiBeginGesture(); };
    link.endGesture = [&p] { p.uiEndGesture(); };
    link.panic = [&p] { p.uiPanic(); };
    link.noteOn = [&p] (int key, float v) { p.uiNoteOn (key, v); };
    link.noteOff = [&p] (int key) { p.uiNoteOff (key); };
    link.heldKeys = [&p] (int word) { return p.engine().heldKeyWord (word); };
    link.track = 0;
    link.modulation = &p;   // la page MOD : la matrice de modulation du plugin
    link.pitchBend = [&p] (float v) { p.uiPitchBend (v); };
    link.modWheel = [&p] (float v) { p.uiModWheel (v); };
    link.outputs = { fm::synthVolume, fm::synthPan, static_cast<ParamId> (sampler::gain), static_cast<ParamId> (sampler::tuneSemitones), static_cast<ParamId> (sampler::tuneCents) };
    content_.reset();
    content_ = std::make_unique<NovaFmEditor> (p, p, std::move (link), palette, def.knob, tr_, opt);
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void NovaFmPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void NovaFmPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
bool NovaFmPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FmSynthProcessor(); }
