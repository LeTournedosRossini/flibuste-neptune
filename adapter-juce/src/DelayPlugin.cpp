#include "DelayPlugin.h"
#include <nlohmann/json.hpp>
#include "NeptuneParameter.h"
#include "neptune/core/DelayModule.h"

using namespace neptune::core;

namespace
{
constexpr int kReserveSlots = 128;
}

namespace
{
constexpr const char* kFormat = "delai-state";

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

juce::AudioProcessor::BusesProperties DelayProcessor::makeBuses()
{
    return BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

DelayProcessor::DelayProcessor()
    : juce::AudioProcessor (makeBuses()), runner_ (std::make_unique<DelayModule>()),
      reserve_ (kReserveSlots,
                [this] (std::uint32_t, std::uint32_t, ParamId id) -> ParamReserve::Ref
                {
                    const auto i = runner_.params().indexOf (id);
                    return i == ParameterSet::npos ? ParamReserve::Ref {} : ParamReserve::Ref { &runner_.params(), i };
                },
                [this] (const ParamReserve::Target& t)
                {
                    const auto* spec = runner_.params().find (t.param);
                    return spec != nullptr ? spec->name : std::string();
                })
{
    jassert (runner_.ok());
    // L'hôte ne voit que les emplacements de la réserve : les réglages du module restent dans l'état du plugin.
    reserve_.addHostParameters (*this, probe_);
    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    for (auto& s : modal_) s = {};
}

bool DelayProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DelayProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    runner_.prepare (sampleRate, samplesPerBlock);
    startTimerHz (20);
}

void DelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n <= 0) return;
    // tempo de l'hôte pour SYNC
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
            if (const auto bpm = pos->getBpm()) static_cast<DelayModule&> (runner_.module()).setTempo (*bpm);
    reserve_.sync();   // les emplacements que l'hôte a bougés règlent leurs réglages avant le traitement
    runner_.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n); // en place ; le bypass est le paramètre du plugin
}

// ---- état : JSON, comme Neptune ({"format", "schema_version", "parameters": {id: valeur réelle}})
void DelayProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    nlohmann::json doc;
    doc["format"] = kFormat;
    doc["schema_version"] = 1;
    doc["module"] = "delay";
    nlohmann::json params = nlohmann::json::object();
    const auto& set = runner_.params();
    for (std::size_t i = 0; i < set.size(); ++i) params[std::to_string (set.spec (i).id)] = set.getAt (i);
    doc["parameters"] = std::move (params);
    doc["reserve"] = reserve_.toJson();
    const auto text = doc.dump (2);
    dest.append (text.data(), text.size());
}

void DelayProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto doc = nlohmann::json::parse (static_cast<const char*> (data), static_cast<const char*> (data) + size, nullptr, false);
    if (doc.is_discarded() || ! doc.is_object()) return;
    const auto fmt = doc.find ("format");
    const auto params = doc.find ("parameters");
    if (fmt == doc.end() || ! fmt->is_string() || fmt->get<std::string>() != kFormat || params == doc.end() || ! params->is_object()) return;
    auto& set = runner_.params();
    set.resetAll();
    for (const auto& [key, value] : params->items())
    {
        if (! value.is_number() || ! std::isfinite (value.get<double>())) continue;
        const long long id = std::strtoll (key.c_str(), nullptr, 10);
        if (id > 0) set.set (static_cast<ParamId> (id), value.get<double>());
    }
    undo_.clear();
    runner_.reset();
    reserve_.fromJson (*this, doc.contains ("reserve") ? doc["reserve"] : nlohmann::json::array());
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

// ---- gestes d'édition (annulation + automation de l'hôte)
void DelayProcessor::uiBeginEdit (ParamId id)
{
    const auto index = runner_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    if (auto* hp = reserve_.hostParameterFor (*this, id)) hp->beginChangeGesture();
    undo_.beginGesture();
}

void DelayProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = runner_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    if (auto* hp = reserve_.hostParameterFor (*this, id)) hp->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void DelayProcessor::uiEndEdit (ParamId id)
{
    const auto index = runner_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    if (auto* hp = reserve_.hostParameterFor (*this, id)) hp->endChangeGesture();
}

void DelayProcessor::uiResetToDefault (ParamId id)
{
    const auto index = runner_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = runner_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool DelayProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    return done;
}

bool DelayProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
    return done;
}

juce::AudioProcessorEditor* DelayProcessor::createEditor() { return new DelayPluginEditor (*this); }

void DelayProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ---- éditeur
DelayPluginEditor::DelayPluginEditor (DelayProcessor& p)
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

void DelayPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto palette = makePalette (p.settings());
    const auto& def = theme::definition (theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark));
    NovaDelayEditor::Options opt;
    opt.brand = true;
    opt.hostTempo = true;
    opt.moduleName = "delay";
    opt.reserveOverlay = true;   // pastille AUTOMATION dans l\'en-tête
    opt.stretchAuto = true;   // fenêtre étirable en largeur
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    // appliqué hors de l'éditeur qui l'appelle : l'interface est détruite puis recréée avec la nouvelle palette
    opt.applySettings = [&p, safe = juce::Component::SafePointer<DelayPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    content_.reset();
    content_ = std::make_unique<NovaDelayEditor> (p, static_cast<const DelayModule*> (&p.runner().module()), palette, def.knob, tr_, opt);
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void DelayPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void DelayPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }

bool DelayPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DelayProcessor(); }

bool DelayProcessor::reserveCanAssign (ParamId id) const
{
    const auto* spec = runner_.params().find (id);
    return spec != nullptr && ! spec->hidden;
}

void DelayProcessor::timerCallback() { reserve_.notifyFollowed (*this); }
