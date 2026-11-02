#include "DiapasonPlugin.h"
#include <nlohmann/json.hpp>
#include "NeptuneParameter.h"

using namespace neptune::core;

namespace
{
constexpr const char* kFormat = "diapason-state";

// Même dossier de configuration que Neptune : le thème, le contraste et la langue sont communs à toute la suite.
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

juce::AudioProcessor::BusesProperties DiapasonProcessor::makeBuses()
{
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

DiapasonProcessor::DiapasonProcessor() : juce::AudioProcessor (makeBuses())
{
    jassert (engine_.ok());
    auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i)
        addParameter (new NeptuneParameter (set, i, probe_)); // l'AudioProcessor en prend la propriété ; même ordre que le jeu
    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    for (auto& s : modal_) s = {};
}

bool DiapasonProcessor::isBusesLayoutSupported (const BusesLayout& l) const { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo(); }

void DiapasonProcessor::prepareToPlay (double sampleRate, int samplesPerBlock) { engine_.prepare (sampleRate, samplesPerBlock); }

void DiapasonProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() < 2) return;
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getWritePointer (1);

    // position de lecture de l'hôte au début du bloc
    generator::Transport tr;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            if (const auto ppq = pos->getPpqPosition()) { tr.valid = true; tr.ppq = *ppq; }
            if (const auto bpm = pos->getBpm()) tr.bpm = *bpm;
            tr.playing = pos->getIsPlaying();
        }
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    auto transportAt = [&tr, sr] (int offset)
    {
        auto t = tr;
        if (t.valid) t.ppq += static_cast<double> (offset) * t.bpm / 60.0 / sr;
        return t;
    };

    // Les événements MIDI sont appliqués à leur position dans le bloc : le rendu est découpé aux événements.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (pos, n, meta.samplePosition);
        if (at > pos)
        {
            engine_.process (left + pos, right + pos, at - pos, transportAt (pos));
            pos = at;
        }
        const auto m = meta.getMessage();
        if (m.isNoteOn()) engine_.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff()) engine_.noteOff (m.getNoteNumber());
        else if (m.isAllNotesOff()) engine_.allNotesOff();
        else if (m.isAllSoundOff()) { engine_.allNotesOff(); engine_.postPanic(); }
    }
    if (pos < n) engine_.process (left + pos, right + pos, n - pos, transportAt (pos));
    midi.clear();

    // messages MIDI envoyés à la main : ils attendent la fin du bloc pour partir vers l'hôte (addOutboundEventsToQueue)
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    midiFifo_.prepareToRead (midiFifo_.getNumReady(), start1, size1, start2, size2);
    auto take = [this] (const MidiPacket& p) { if (p.size > 0 && numHostMidi_ < static_cast<int> (hostMidi_.size())) hostMidi_[static_cast<std::size_t> (numHostMidi_++)] = p; };
    for (int i = 0; i < size1; ++i) take (midiPackets_[static_cast<std::size_t> (start1 + i)]);
    for (int i = 0; i < size2; ++i) take (midiPackets_[static_cast<std::size_t> (start2 + i)]);
    midiFifo_.finishedRead (size1 + size2);
}

bool DiapasonProcessor::postMidi (const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > 1024) return false;
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    midiFifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 < 1) return false;
    auto& p = midiPackets_[static_cast<std::size_t> (size1 > 0 ? start1 : start2)];
    p.size = static_cast<std::uint16_t> (bytes.size());
    std::copy (bytes.begin(), bytes.end(), p.bytes.begin());
    midiFifo_.finishedWrite (1);
    return true;
}

void DiapasonProcessor::addOutboundEventsToQueue (const clap_output_events* out, const juce::MidiBuffer&, int sampleOffset)
{
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
}

// ---- état : JSON, comme Neptune ({"format", "schema_version", "parameters": {id: valeur réelle}})
void DiapasonProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    nlohmann::json doc;
    doc["format"] = kFormat;
    doc["schema_version"] = 1;
    doc["module"] = "diapason";
    nlohmann::json params = nlohmann::json::object();
    const auto& set = engine_.params();
    for (std::size_t i = 0; i < set.size(); ++i) params[std::to_string (set.spec (i).id)] = set.getAt (i);
    doc["parameters"] = std::move (params);
    const auto text = doc.dump (2);
    dest.append (text.data(), text.size());
}

void DiapasonProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto doc = nlohmann::json::parse (static_cast<const char*> (data), static_cast<const char*> (data) + size, nullptr, false);
    if (doc.is_discarded() || ! doc.is_object()) return;
    const auto fmt = doc.find ("format");
    const auto params = doc.find ("parameters");
    if (fmt == doc.end() || ! fmt->is_string() || fmt->get<std::string>() != kFormat || params == doc.end() || ! params->is_object()) return;
    auto& set = engine_.params();
    set.resetAll();
    for (const auto& [key, value] : params->items())
    {
        if (! value.is_number() || ! std::isfinite (value.get<double>())) continue;
        const long long id = std::strtoll (key.c_str(), nullptr, 10);
        if (id > 0) set.set (static_cast<ParamId> (id), value.get<double>());
    }
    undo_.clear();
    engine_.reset();
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

// ---- gestes d'édition (annulation + automation de l'hôte)
void DiapasonProcessor::uiBeginEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    getParameters()[static_cast<int> (index)]->beginChangeGesture();
    undo_.beginGesture();
}

void DiapasonProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = engine_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    getParameters()[static_cast<int> (index)]->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void DiapasonProcessor::uiEndEdit (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    getParameters()[static_cast<int> (index)]->endChangeGesture();
}

void DiapasonProcessor::uiResetToDefault (ParamId id)
{
    const auto index = engine_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = engine_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool DiapasonProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    return done;
}

bool DiapasonProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    return done;
}

juce::AudioProcessorEditor* DiapasonProcessor::createEditor() { return new DiapasonPluginEditor (*this); }

void DiapasonProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ---- éditeur
DiapasonPluginEditor::DiapasonPluginEditor (DiapasonProcessor& p)
    : juce::AudioProcessorEditor (p), processor_ (p), tr_ (p.settings().language)
{
    buildContent();
    setWantsKeyboardFocus (true);
    // proportions de la maquette conservées ; l'interface se redessine à la vraie taille (aucune transformation)
    nova::configureStretchConstrainer (constrainer_, nova::EditorBase::designWidth, nova::EditorBase::designHeight, nova::EditorBase::minScale, nova::EditorBase::maxScale, 4096);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (nova::EditorBase::designWidth, nova::EditorBase::designHeight);
}

void DiapasonPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto palette = makePalette (p.settings());
    const auto& def = theme::definition (theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark));
    NovaDiapasonEditor::Options opt;
    opt.brand = true;
    opt.moduleName = "diapason";
    opt.stretchAuto = true;   // fenêtre étirable en largeur
    opt.noPresets = true;
    opt.noAB = true;
    opt.noBypass = true;
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    // appliqué hors de l'éditeur qui l'appelle : l'interface est détruite puis recréée avec la nouvelle palette
    opt.applySettings = [&p, safe = juce::Component::SafePointer<DiapasonPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    content_.reset();
    content_ = std::make_unique<NovaDiapasonEditor> (p, p.engine(), palette, def.knob, tr_, opt);
    content_->onSendMidi = [&p] (const std::vector<std::uint8_t>& bytes) { return p.postMidi (bytes); };
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void DiapasonPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void DiapasonPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }

bool DiapasonPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DiapasonProcessor(); }
