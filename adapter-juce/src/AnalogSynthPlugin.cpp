#include "AnalogSynthPlugin.h"
#include "NovaSynthEditor.h"
#include <nlohmann/json.hpp>
#include "NeptuneParameter.h"
#include "neptune/core/SamplerParams.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace analog = neptune::core::analogsynth;

namespace
{
// Même dossier de configuration que Neptune : le thème, le contraste et la langue sont communs à toute la suite.
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

juce::AudioProcessor::BusesProperties AnalogSynthProcessor::makeBuses()
{
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

AnalogSynthProcessor::AnalogSynthProcessor() : juce::AudioProcessor (makeBuses())
{
    jassert (engine_.ok());
    auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i)
        addParameter (new NeptuneParameter (set, i, probe_)); // l'AudioProcessor en prend la propriété ; même ordre que le jeu

    std::vector<std::string> warnings;
    const auto dir = configDirectory();
    settings_ = settings::loadFile (dir / "settings.json", &warnings);

    // Bibliothèque de presets : dossier choisi dans les réglages, sinon <config>/AnalogSynth.
    std::filesystem::path root = settings_.presetsDir.empty() ? dir / "AnalogSynth" : std::filesystem::u8path (settings_.presetsDir);
    library_ = std::make_unique<preset::Library> (std::move (root), analog::factoryPresets());
    library_->open (&warnings);
    if (library_->config().parameterCaptureDefaults.empty()) library_->setConfig (analog::defaultConfig());
    for (auto& s : modal_) s = {};
}

bool AnalogSynthProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void AnalogSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    rig_.prepare (sampleRate);
    rig_.reset (engine_.params());
}

void AnalogSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 1) return;
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Molettes de l'écran : une position qui change est transmise comme un message MIDI (hauteur du synthé, source de la matrice)
    {
        const float bend = uiBend_.load (std::memory_order_relaxed), mod = uiMod_.load (std::memory_order_relaxed);
        if (bend != lastUiBend_)
        {
            lastUiBend_ = bend;
            engine_.setPitchBend (bend);
            rig_.midiEvent ({ midi::MsgKind::PitchBend, 0, 0, juce::jlimit (0, 127, static_cast<int> (std::lround (64.0f + bend * 63.5f))) });
        }
        if (mod != lastUiMod_)
        {
            lastUiMod_ = mod;
            rig_.midiEvent ({ midi::MsgKind::ControlChange, 0, 1, juce::jlimit (0, 127, static_cast<int> (std::lround (mod * 127.0f))) });
        }
    }

    // Matrice de modulation : évaluée une fois par bloc, avant le rendu (les décalages sont lus par le moteur à chaque morceau de bloc)
    rig_.process (n, engine_.params());

    // Les événements MIDI sont appliqués à leur position dans le bloc : le rendu est découpé aux événements.
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
        else if (m.isController()) rig_.midiEvent ({ midi::MsgKind::ControlChange, channel, m.getControllerNumber(), m.getControllerValue() });
        else if (m.isPitchWheel())
        {
            rig_.midiEvent ({ midi::MsgKind::PitchBend, channel, 0, m.getPitchWheelValue() >> 7 });
            engine_.setPitchBend (static_cast<float> (m.getPitchWheelValue() - 8192) / 8192.0f);   // le synthé suit la molette (plage du patch)
        }
        else if (m.isChannelPressure()) rig_.midiEvent ({ midi::MsgKind::ChannelPressure, channel, 0, m.getChannelPressureValue() });
        else if (m.isAftertouch()) rig_.midiEvent ({ midi::MsgKind::PolyPressure, channel, m.getNoteNumber(), m.getAfterTouchValue() });
        if (m.isAllNotesOff()) { engine_.releaseAll(); rig_.releaseAll(); }
        else if (m.isAllSoundOff()) engine_.panic();
        else if (m.isController() && m.getControllerNumber() == 0) midiBank_.store (juce::jlimit (0, 127, m.getControllerValue()));
        else if (m.isProgramChange())
        {
            pendingBank_.store (midiBank_.load());
            pendingProgram_.store (m.getProgramChangeNumber());
            triggerAsyncUpdate(); // le chargement d'un preset se fait sur le thread de message
        }
    }
    if (pos < n) engine_.process (left + pos, right != nullptr ? right + pos : nullptr, n - pos);
    midi.clear();
}

// Program Change reçu : emplacement vide -> le preset actuel reste (aucune interruption, aucun choix demandé en concert).
void AnalogSynthProcessor::handleAsyncUpdate()
{
    const int bank = pendingBank_.exchange (-1), program = pendingProgram_.exchange (-1);
    if (bank < 0 || program < 0) return;
    const auto id = preset::Navigator::presetAt (library_->banks(), { bank, program });
    if (! id.empty()) loadPreset (id);
}

// ---- état : JSON, comme Neptune
void AnalogSynthProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const auto matrix = rig_.matrix();
    const auto text = analog::stateToJson (engine_.params(), engine_.description(), currentPresetId_, &matrix);
    dest.append (text.data(), text.size());
}

void AnalogSynthProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    SynthDescription d;
    std::string presetId;
    modulation::Matrix matrix;
    const std::string text (static_cast<const char*> (data), static_cast<std::size_t> (size));
    if (! analog::stateFromJson (text, engine_.params(), d, presetId, &matrix)) return;
    engine_.setDescription (d);
    rig_.setMatrix (matrix, engine_.params());
    currentPresetId_ = presetId;
    undo_.clear();
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

// ---- gestes d'édition (annulation + automation de l'hôte)
void AnalogSynthProcessor::uiBeginEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    getParameters()[static_cast<int> (index)]->beginChangeGesture();
    undo_.beginGesture();
}

void AnalogSynthProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = engine_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    getParameters()[static_cast<int> (index)]->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void AnalogSynthProcessor::uiEndEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    getParameters()[static_cast<int> (index)]->endChangeGesture();
}

void AnalogSynthProcessor::uiResetToDefault (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = engine_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool AnalogSynthProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

bool AnalogSynthProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return done;
}

void AnalogSynthProcessor::uiEditSynth (SynthDescription d, std::string name, std::string mergeKey)
{
    d = sanitised (std::move (d));
    const auto previous = engine_.description();
    if (previous == d) return;
    undo_.perform (std::make_unique<SnapshotCommand<SynthDescription>> (
        previous, d,
        [this] (const SynthDescription& v)
        {
            const auto before = snapshotNormalised();
            engine_.setDescription (v); // recopie aussi les temps d'enveloppe, le glissando et l'équilibre dans les paramètres
            notifyHostOfParameterChanges (before);
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged (true));
        },
        std::move (name), std::move (mergeKey)));
}

// ---- matrice de modulation (onglet MOD)
void AnalogSynthProcessor::modEdit (int, modulation::Matrix matrix)
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
std::string AnalogSynthProcessor::modulationText() const
{
    const auto& m = rig_.matrix();
    return m == modulation::Matrix {} ? std::string() : modulation::toJson (m);
}

std::string AnalogSynthProcessor::modDestName (int, ParamId dest) const
{
    const auto& set = engine_.params();
    const auto index = set.indexOf (dest);
    return index == ParameterSet::npos ? std::string() : set.spec (index).name;
}

// ---- presets
std::vector<float> AnalogSynthProcessor::snapshotNormalised() const
{
    std::vector<float> v;
    const auto& set = engine_.params();
    v.reserve (set.size());
    for (std::size_t i = 0; i < set.size(); ++i) v.push_back (static_cast<float> (set.getNormalizedAt (i)));
    return v;
}

// Les valeurs ont changé hors des gestes de l'interface : l'hôte est prévenu pour chaque paramètre modifié.
void AnalogSynthProcessor::notifyHostOfParameterChanges (const std::vector<float>& before)
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

bool AnalogSynthProcessor::loadPreset (const std::string& id)
{
    const auto* p = library_->find (id);
    if (p == nullptr) return false;
    const auto before = snapshotNormalised();
    if (! engine_.loadPreset (*p, library_->config())) return false; // capture par voix : voir AnalogSynthEngine::loadPreset
    rig_.setMatrix (p->modulation.empty() ? modulation::Matrix {} : modulation::fromJson (p->modulation), engine_.params());   // un preset sans matrice la vide
    currentPresetId_ = id;
    undo_.clear();
    notifyHostOfParameterChanges (before);
    ++presetRevision_;
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true).withNonParameterStateChanged (true));
    return true;
}

bool AnalogSynthProcessor::stepPreset (int direction)
{
    const auto next = library_->stepFrom (currentPresetId_, direction, [this] (const preset::Preset& p) { return acceptsPreset (p); });
    return ! next.empty() && loadPreset (next);
}

bool AnalogSynthProcessor::isModified() const
{
    const auto* p = currentPreset();
    if (p == nullptr) return false;
    const auto stored = p->modulation.empty() ? modulation::Matrix {} : modulation::sanitised (modulation::fromJson (p->modulation));
    return analog::differsFromPreset (*p, engine_.params(), engine_.description(), library_->config()) || stored != rig_.matrix();
}

bool AnalogSynthProcessor::canSaveCurrent() const
{
    const auto* p = currentPreset();
    return p != nullptr && ! p->factory && ! p->locked;
}

bool AnalogSynthProcessor::overwritePreset (const std::string& id, std::string* error)
{
    const auto* base = library_->find (id);
    if (base == nullptr || base->factory) { if (error) *error = "factory preset is read-only"; return false; }
    if (base->locked) { if (error) *error = "preset is locked"; return false; }
    auto p = analog::capturePreset (engine_.params(), engine_.description(), library_->config(), base->name, base->type, base);
    p.modulation = modulationText();
    const bool ok = library_->save (std::move (p), error);
    if (ok) ++presetRevision_;
    return ok;
}

bool AnalogSynthProcessor::saveCurrent (std::string* error)
{
    if (currentPresetId_.empty()) { if (error) *error = "no current preset"; return false; }
    return overwritePreset (currentPresetId_, error);
}

bool AnalogSynthProcessor::saveCurrentAs (const std::string& name, const std::string& type, preset::Preset* out, std::string* error)
{
    auto p = analog::capturePreset (engine_.params(), engine_.description(), library_->config(), name, type);
    if (const auto* cur = currentPreset())
    {
        p.characteristics = cur->characteristics;
        p.customFields = cur->customFields;
        p.rating = cur->rating;
        p.captureOverrides = cur->captureOverrides;
        if (p.type.empty()) p.type = cur->type;
        p.derivedFrom = cur->factory ? cur->id : cur->derivedFrom;
        // les exceptions de capture reviennent dans les paramètres stockés
        p = analog::capturePreset (engine_.params(), engine_.description(), library_->config(), name, p.type, &p);
    }
    p.modulation = modulationText();
    preset::Preset stored;
    if (! library_->importPreset (std::move (p), &stored, error)) return false;
    currentPresetId_ = stored.id;
    ++presetRevision_;
    if (out != nullptr) *out = stored;
    return true;
}

bool AnalogSynthProcessor::saveConfig (const preset::LibraryConfig& c)
{
    const bool ok = library_->setConfig (c);
    if (ok) ++presetRevision_;
    return ok;
}

juce::AudioProcessorEditor* AnalogSynthProcessor::createEditor() { return new NovaSynthPluginEditor (*this); }

void AnalogSynthProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ------------------------------------------------------------------------------------------------ fenêtre du plugin
NovaSynthPluginEditor::NovaSynthPluginEditor (AnalogSynthProcessor& p) : juce::AudioProcessorEditor (p), processor_ (p), tr_ (p.settings().language)
{
    buildContent();
    setWantsKeyboardFocus (true);
    nova::configureStretchConstrainer (constrainer_, nova::EditorBase::designWidth, nova::EditorBase::designHeight, nova::EditorBase::minScale, nova::EditorBase::maxScale, 4096);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (nova::EditorBase::designWidth, nova::EditorBase::designHeight);
}

void NovaSynthPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto id = theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark);
    const auto palette = theme::withContrast (theme::definition (id), p.settings().contrast);
    const auto& def = theme::definition (id);
    NovaSynthEditor::Options opt;
    opt.brand = true;
    opt.stretchAuto = true;   // fenêtre étirable en largeur
    opt.noBypass = true;
    opt.moduleName = "analogsynth";
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    opt.applySettings = [&p, safe = juce::Component::SafePointer<NovaSynthPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    opt.presetMenu = [this] (juce::Component* anchor) { if (content_ != nullptr) content_->showPresetMenu (anchor); };
    SynthLink link;
    link.description = [&p] { return p.synthDescription(); };
    link.edit = [&p] (SynthDescription d) { p.uiEditSynth (std::move (d), "Synth", "synth"); };
    link.beginGesture = [&p] { p.uiBeginGesture(); };
    link.endGesture = [&p] { p.uiEndGesture(); };
    link.panic = [&p] { p.uiPanic(); };
    link.noteOn = [&p] (int key, float v) { p.uiNoteOn (key, v); };
    link.noteOff = [&p] (int key) { p.uiNoteOff (key); };
    link.heldKeys = [&p] (int word) { return p.engine().heldKeyWord (word); };
    link.track = 0;
    link.pitchBend = [&p] (float v) { p.uiPitchBend (v); };
    link.modWheel = [&p] (float v) { p.uiModWheel (v); };
    link.modulation = &p;   // la page MOD : la matrice de modulation du plugin
    link.outputs = { analog::volume, analog::pan, static_cast<ParamId> (sampler::gain), static_cast<ParamId> (sampler::tuneSemitones), static_cast<ParamId> (sampler::tuneCents) };
    content_.reset();
    content_ = std::make_unique<NovaSynthEditor> (p, p, std::move (link), palette, def.knob, tr_, opt);
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void NovaSynthPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void NovaSynthPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
bool NovaSynthPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AnalogSynthProcessor(); }
