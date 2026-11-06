#include "TridentPlugin.h"

#include <cmath>
#include <nlohmann/json.hpp>

#include "NeptuneParameter.h"
#include "NovaChorusEditor.h"
#include "NovaCompressorEditor.h"
#include "NovaConvolutionEditor.h"
#include "NovaDelayEditor.h"
#include "NovaDiapasonEditor.h"
#include "NovaDigueEditor.h"
#include "NovaEqualizerEditor.h"
#include "NovaFournaiseEditor.h"
#include "NovaMicrodecaleEditor.h"
#include "NovaShaperEditor.h"
#include "UtilityEditor.h"
#include "neptune/core/ChorusModule.h"
#include "neptune/core/CompressorModule.h"
#include "neptune/core/ConvolutionModule.h"
#include "neptune/core/Crossover.h"
#include "neptune/core/DelayModule.h"
#include "neptune/core/DiapasonModule.h"
#include "neptune/core/EqualizerModule.h"
#include "neptune/core/LimiterModule.h"
#include "neptune/core/MicrodecaleModule.h"
#include "neptune/core/OverdriveModule.h"
#include "neptune/core/ShaperModule.h"

using namespace neptune::core;
using namespace neptune::core::trident;
using neptune::core::theme::Token;

namespace
{
constexpr const char* kFormat = "trident-state";

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

juce::String U (const char* t) { return juce::String::fromUTF8 (t); }
} // namespace

// ================================================================================================ processeur

juce::AudioProcessor::BusesProperties TridentProcessor::makeBuses()
{
    return BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

TridentProcessor::TridentProcessor() : juce::AudioProcessor (makeBuses())
{
    jassert (trident_.ok());
    auto& set = trident_.params();
    hostIndex_.assign (set.size(), -1);
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        hostIndex_[i] = getParameters().size();
        addParameter (new NeptuneParameter (set, i, probe_));   // l'AudioProcessor en prend la propriété
    }
    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    for (auto& s : modal_) s = {};
}

TridentProcessor::~TridentProcessor() { stopTimer(); }

bool TridentProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void TridentProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    trident_.prepare (sampleRate, samplesPerBlock);
    declaredLatency_ = trident_.latencySamples();
    setLatencySamples (declaredLatency_);
    startTimerHz (20);
}

// La latence totale dépend des effets posés et de leurs réglages : on la redéclare à l'hôte quand elle change.
void TridentProcessor::timerCallback()
{
    trident_.collect();
    // un réglage assigné qu'on a bougé à l'écran : son emplacement l'a suivi, l'hôte doit le savoir (affichage, écriture d'automation)
    for (int k = 0; k < kAssignables; ++k)
        if (trident_.takeFollowed (k))
        {
            const auto i = trident_.params().indexOf (pid::assign (k));
            if (i != neptune::core::ParameterSet::npos && hostIndex_[i] >= 0)
                if (auto* p = getParameters()[hostIndex_[i]]) p->setValueNotifyingHost (p->getValue());
        }
    const int latency = trident_.latencySamples();
    if (latency == declaredLatency_) return;
    declaredLatency_ = latency;
    setLatencySamples (latency);
}

void TridentProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n <= 0) return;
    trident_.process (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
}

void TridentProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const auto text = trident_.stateToJson();
    dest.append (text.data(), text.size());
}

void TridentProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const std::string text (static_cast<const char*> (data), static_cast<std::size_t> (size));
    if (! trident_.stateFromJson (text)) return;
    undo_.clear();
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

void TridentProcessor::setEffect (int path, int slot, const std::string& type)
{
    trident_.setEffect (path, slot, type);
    notifyStateChanged();
}

void TridentProcessor::notifyStateChanged() { updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true)); }

// ---- gestes d'édition (annulation + automation de l'hôte)
void TridentProcessor::uiBeginEdit (ParamId id)
{
    const auto index = trident_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    getParameters()[hostIndex_[index]]->beginChangeGesture();
    undo_.beginGesture();
}

void TridentProcessor::uiSetNormalised (ParamId id, double n)
{
    auto& set = trident_.params();
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.setParameter (set, id, set.spec (index).fromNormalized (n));
    getParameters()[hostIndex_[index]]->setValueNotifyingHost (static_cast<float> (set.getNormalizedAt (index)));
}

void TridentProcessor::uiEndEdit (ParamId id)
{
    const auto index = trident_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    undo_.endGesture();
    getParameters()[hostIndex_[index]]->endChangeGesture();
}

void TridentProcessor::uiResetToDefault (ParamId id)
{
    const auto index = trident_.params().indexOf (id);
    if (index == ParameterSet::npos) return;
    const auto& spec = trident_.params().spec (index);
    uiBeginEdit (id);
    uiSetNormalised (id, spec.toNormalized (spec.defaultValue));
    uiEndEdit (id);
}

bool TridentProcessor::uiUndo()
{
    const bool done = undo_.undo();
    if (done) notifyStateChanged();
    return done;
}

bool TridentProcessor::uiRedo()
{
    const bool done = undo_.redo();
    if (done) notifyStateChanged();
    return done;
}

std::string TridentProcessor::uiText (std::string_view key) const { return std::string (i18n::Translator (settings_.language).tr (key)); }

bool TridentProcessor::canAssignMacro (ParamId id) const { return id < pid::assignBase; }

std::vector<int> TridentProcessor::macrosOf (ParamId id) const
{
    std::vector<int> out;
    for (int k = 0; k < kAssignables; ++k)
    {
        const auto t = trident_.assignment (k);
        if (t.active && t.path < 0 && t.param == id) out.push_back (k);
    }
    return out;
}

void TridentProcessor::uiToggleMacro (ParamId id, int macro)
{
    if (! canAssignMacro (id) || macro < 0 || macro >= kAssignables) return;
    const auto t = trident_.assignment (macro);
    if (t.active && t.path < 0 && t.param == id) trident_.unassign (macro);
    else trident_.assign (macro, -1, 0, id);
    notifyStateChanged();
}

void TridentProcessor::uiClearMacros (ParamId id)
{
    for (const int k : macrosOf (id)) trident_.unassign (k);
    notifyStateChanged();
}

juce::AudioProcessorEditor* TridentProcessor::createEditor() { return new TridentPluginEditor (*this); }

void TridentProcessor::applySettings (const settings::GlobalSettings& s)
{
    settings_ = s;
    std::string error;
    settings::saveFile (configDirectory() / "settings.json", s, &error);
}

// ================================================================================================ éditeur d'un effet (superposé à la fenêtre)

namespace
{
// Les réglages d'un effet posé dans un emplacement : annulables, avec les macros de Trident.
class SlotHost : public ParamHost
{
public:
    SlotHost (TridentProcessor& owner, int path, int slot, std::shared_ptr<InsertInstance> inst, const i18n::Translator& tr)
        : owner_ (owner), path_ (path), slot_ (slot), inst_ (std::move (inst)), tr_ (tr) {}

    ParameterSet& parameters() override { return inst_->runner.params(); }
    void uiBeginEdit (ParamId) override { owner_.undo().beginGesture(); }
    void uiSetNormalised (ParamId id, double n) override
    {
        auto& ps = parameters();
        const auto index = ps.indexOf (id);
        if (index == ParameterSet::npos) return;
        owner_.undo().setParameter (ps, id, ps.spec (index).fromNormalized (n));
        owner_.notifyStateChanged();
    }
    void uiEndEdit (ParamId) override { owner_.undo().endGesture(); }
    void uiResetToDefault (ParamId id) override
    {
        auto& ps = parameters();
        const auto index = ps.indexOf (id);
        if (index == ParameterSet::npos) return;
        uiBeginEdit (id);
        uiSetNormalised (id, ps.spec (index).toNormalized (ps.spec (index).defaultValue));
        uiEndEdit (id);
    }
    void uiEditText (std::function<void (const std::string&)> apply, std::string oldValue, std::string newValue, std::string name) override
    {
        if (oldValue == newValue) return;
        owner_.undo().perform (std::make_unique<SnapshotCommand<std::string>> (std::move (oldValue), std::move (newValue), std::move (apply), std::move (name)));
        owner_.notifyStateChanged();
    }
    std::string uiText (std::string_view key) const override { return std::string (tr_.tr (key)); }
    bool uiUndo() override { return owner_.uiUndo(); }
    bool uiRedo() override { return owner_.uiRedo(); }
    neptune::core::uistate::Size modalSize (neptune::core::uistate::Terminal t) const override { return owner_.modalSize (t); }
    void setModalSize (neptune::core::uistate::Terminal t, neptune::core::uistate::Size s) override { owner_.setModalSize (t, s); }

    bool canAssignMacro (ParamId id) const override { return id != stock::bypass; }
    std::vector<int> macrosOf (ParamId id) const override
    {
        std::vector<int> out;
        for (int k = 0; k < kAssignables; ++k)
        {
            const auto t = owner_.trident().assignment (k);
            if (t.active && t.path == path_ && t.slot == slot_ && t.param == id) out.push_back (k);
        }
        return out;
    }
    void uiToggleMacro (ParamId id, int macro) override
    {
        if (macro < 0 || macro >= kAssignables || ! canAssignMacro (id)) return;
        const auto t = owner_.trident().assignment (macro);
        if (t.active && t.path == path_ && t.slot == slot_ && t.param == id) owner_.trident().unassign (macro);
        else owner_.trident().assign (macro, path_, slot_, id);
        owner_.notifyStateChanged();
    }
    void uiClearMacros (ParamId id) override
    {
        for (const int k : macrosOf (id)) owner_.trident().unassign (k);
        owner_.notifyStateChanged();
    }

private:
    TridentProcessor& owner_;
    int path_, slot_;
    std::shared_ptr<InsertInstance> inst_;
    const i18n::Translator& tr_;
};

class SlotOverlay : public juce::Component, private juce::Timer
{
public:
    SlotOverlay (TridentProcessor& p, int path, int slot, const theme::Palette& palette, const i18n::Translator& tr, theme::KnobStyle knobStyle, std::function<void()> onClose)
        : processor_ (p), path_ (path), slot_ (slot), palette_ (palette), tr_ (tr), onClose_ (std::move (onClose))
    {
        setWantsKeyboardFocus (true);
        inst_ = processor_.trident().effect (path, slot);
        if (inst_ == nullptr) return;
        host_ = std::make_unique<SlotHost> (processor_, path, slot, inst_, tr_);
        const auto rate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        const std::string& type = inst_->type;
        auto& module = inst_->runner.module();
        if (type == "diapason")
        {
            NovaDiapasonEditor::Options opt;
            opt.moduleName = "diapason";
            opt.sampleRate = rate;
            diapason_ = std::make_unique<NovaDiapasonEditor> (*host_, static_cast<DiapasonModule*> (&module)->engine(), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*diapason_);
        }
        else if (type == "limiter")
        {
            NovaDigueEditor::Options opt;
            opt.moduleName = "digue";
            opt.sampleRate = rate;
            limiter_ = std::make_unique<NovaDigueEditor> (*host_, static_cast<const LimiterModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*limiter_);
        }
        else if (type == "overdrive")
        {
            NovaFournaiseEditor::Options opt;
            opt.moduleName = "fournaise";
            opt.sampleRate = rate;
            overdrive_ = std::make_unique<NovaFournaiseEditor> (*host_, static_cast<const OverdriveModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*overdrive_);
        }
        else if (type == "convolution")
        {
            NovaConvolutionEditor::Options opt;
            opt.moduleName = "convolution";
            opt.sampleRate = rate;
            conv_ = std::make_unique<NovaConvolutionEditor> (*host_, static_cast<ConvolutionModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*conv_);
        }
        else if (type == "microdecale")
        {
            NovaMicrodecaleEditor::Options opt;
            opt.moduleName = "microdecale";
            opt.sampleRate = rate;
            micro_ = std::make_unique<NovaMicrodecaleEditor> (*host_, static_cast<const MicrodecaleModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*micro_);
        }
        else if (type == "chorus")
        {
            NovaChorusEditor::Options opt;
            opt.moduleName = "chorus";
            opt.sampleRate = rate;
            chorus_ = std::make_unique<NovaChorusEditor> (*host_, static_cast<const ChorusModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*chorus_);
        }
        else if (type == "delay")
        {
            NovaDelayEditor::Options opt;
            opt.moduleName = "delay";
            opt.sampleRate = rate;
            delay_ = std::make_unique<NovaDelayEditor> (*host_, static_cast<const DelayModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*delay_);
        }
        else if (type == "equalizer")
        {
            NovaEqualizerEditor::Options opt;
            opt.moduleName = "equalizer";
            opt.sampleRate = rate;
            equalizer_ = std::make_unique<NovaEqualizerEditor> (*host_, static_cast<const EqualizerModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*equalizer_);
        }
        else if (type == "compressor")
        {
            NovaCompressorEditor::Options opt;
            opt.moduleName = "compressor";
            opt.sampleRate = rate;
            compressor_ = std::make_unique<NovaCompressorEditor> (*host_, static_cast<const CompressorModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*compressor_);
        }
        else if (type == "shaper")
        {
            NovaShaperEditor::Options opt;
            opt.moduleName = "shaper";
            opt.sampleRate = rate;
            shaper_ = std::make_unique<NovaShaperEditor> (*host_, static_cast<const ShaperModule*> (&module), palette_, knobStyle, tr_, opt);
            addAndMakeVisible (*shaper_);
        }
        else
        {
            UtilityEditor::Options opt;
            opt.sampleRate = rate;
            utility_ = std::make_unique<UtilityEditor> (*host_, &module, palette_, tr_, knobStyle, opt);
            addAndMakeVisible (*utility_);
        }
        title_ = juce::String::fromUTF8 (std::string (tr_.tr (std::string ("module.") + type)).c_str()) + U ("  -  chemin ") + juce::String (path + 1) + U ("  -  emplacement ") + juce::String (slot + 1);
        bypassButton_.setButtonText (juce::String::fromUTF8 (std::string (tr_.tr ("insert.bypass")).c_str()));
        bypassButton_.onClick = [this]
        {
            auto& ps = inst_->runner.params();
            const bool on = ps.get (stock::bypass) < 0.5;
            host_->uiBeginEdit (stock::bypass);
            host_->uiSetNormalised (stock::bypass, ps.find (stock::bypass)->toNormalized (on ? 1.0 : 0.0));
            host_->uiEndEdit (stock::bypass);
        };
        addAndMakeVisible (bypassButton_);
        closeButton_.setButtonText (juce::String::fromUTF8 (std::string (tr_.tr ("insert.close")).c_str()));
        closeButton_.onClick = [this] { if (onClose_) onClose_(); };
        addAndMakeVisible (closeButton_);
        startTimerHz (20);
    }
    ~SlotOverlay() override { stopTimer(); }
    bool valid() const { return inst_ != nullptr; }

    void paint (juce::Graphics& g) override
    {
        const auto col = [this] (Token t) { const auto& c = palette_.color (t); return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a)); };
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (col (Token::BgApp));
        g.fillRoundedRectangle (panel_.toFloat(), 8.0f);
        g.setColour (col (Token::Accent));
        g.drawRoundedRectangle (panel_.toFloat(), 8.0f, 1.5f);
        g.setColour (col (Token::Text));
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText (title_, panel_.getX() + 16, panel_.getY() + 10, panel_.getWidth() - 240, 36, juce::Justification::centredLeft);
    }
    void resized() override
    {
        const int w = juce::jmin (getWidth() - 40, 2400), h = juce::jmin (getHeight() - 40, 1700);
        panel_ = juce::Rectangle<int> (0, 0, w, h).withCentre (getLocalBounds().getCentre());
        auto r = panel_.reduced (10);
        auto head = r.removeFromTop (36);
        closeButton_.setBounds (head.removeFromRight (90).reduced (2));
        bypassButton_.setBounds (head.removeFromRight (110).reduced (2));
        r.removeFromTop (4);
        for (juce::Component* c : std::initializer_list<juce::Component*> { utility_.get(), shaper_.get(), equalizer_.get(), delay_.get(), chorus_.get(), micro_.get(), conv_.get(), overdrive_.get(), limiter_.get(), diapason_.get(), compressor_.get() })
            if (c != nullptr) c->setBounds (r);
    }
    void mouseDown (const juce::MouseEvent& e) override { if (! panel_.contains (e.getPosition()) && onClose_) onClose_(); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onClose_) onClose_(); return true; }
        for (juce::Component* c : std::initializer_list<juce::Component*> { utility_.get(), shaper_.get(), equalizer_.get(), delay_.get(), chorus_.get(), micro_.get(), conv_.get(), overdrive_.get(), limiter_.get(), diapason_.get(), compressor_.get() })
            if (c != nullptr) return c->keyPressed (k);
        return false;
    }

private:
    void timerCallback() override
    {
        bypassButton_.setToggleState (inst_->runner.params().get (stock::bypass) >= 0.5, juce::dontSendNotification);
        // l'emplacement a été vidé ou remplacé pendant que l'éditeur était ouvert : on ferme
        if (processor_.trident().effect (path_, slot_) != inst_ && onClose_) onClose_();
    }

    TridentProcessor& processor_;
    int path_, slot_;
    theme::Palette palette_;
    const i18n::Translator& tr_;
    std::function<void()> onClose_;
    std::shared_ptr<InsertInstance> inst_;
    std::unique_ptr<SlotHost> host_;
    std::unique_ptr<UtilityEditor> utility_;
    std::unique_ptr<NovaShaperEditor> shaper_;
    std::unique_ptr<NovaEqualizerEditor> equalizer_;
    std::unique_ptr<NovaDelayEditor> delay_;
    std::unique_ptr<NovaChorusEditor> chorus_;
    std::unique_ptr<NovaMicrodecaleEditor> micro_;
    std::unique_ptr<NovaConvolutionEditor> conv_;
    std::unique_ptr<NovaFournaiseEditor> overdrive_;
    std::unique_ptr<NovaDigueEditor> limiter_;
    std::unique_ptr<NovaDiapasonEditor> diapason_;
    std::unique_ptr<NovaCompressorEditor> compressor_;
    juce::TextButton bypassButton_, closeButton_;
    juce::String title_;
    juce::Rectangle<int> panel_;
};
} // namespace

// ================================================================================================ interface NOVA

namespace
{
juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0)   // un trait qui se sépare en trois
    {
        p.startNewSubPath (2.0f, 12.0f); p.lineTo (9.0f, 12.0f);
        p.startNewSubPath (9.0f, 12.0f); p.lineTo (14.0f, 5.0f); p.lineTo (22.0f, 5.0f);
        p.startNewSubPath (9.0f, 12.0f); p.lineTo (22.0f, 12.0f);
        p.startNewSubPath (9.0f, 12.0f); p.lineTo (14.0f, 19.0f); p.lineTo (22.0f, 19.0f);
    }
    else if (kind == 1)   // des curseurs
    {
        for (int i = 0; i < 3; ++i)
        {
            const float x = 5.0f + 7.0f * static_cast<float> (i);
            p.startNewSubPath (x, 3.0f); p.lineTo (x, 21.0f);
            p.addEllipse (x - 2.5f, 6.0f + 4.0f * static_cast<float> (i), 5.0f, 5.0f);
        }
    }
    else
    {
        p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
        p.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
    }
    return p;
}

double hzToX (double hz) { return std::log (hz / 20.0) / std::log (1000.0); }   // 20 Hz -> 0, 20 kHz -> 1
double xToHz (double x) { return 20.0 * std::pow (1000.0, std::clamp (x, 0.0, 1.0)); }
float dbOf (float lin) { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }
} // namespace

class NovaTridentEditor : public nova::EditorBase
{
public:
    NovaTridentEditor (TridentProcessor&, const theme::Palette&, theme::KnobStyle, const i18n::Translator&, Options);
    ~NovaTridentEditor() override;

private:
    class CrossView;
    void layout() override;
    void tick() override;
    void applyRouting (int mode);
    void showEffectMenu (int path, int slot, juce::Component* anchor);
    void openSlot (int path, int slot);
    void closeOverlay();
    juce::String effectName (int path, int slot) const;
    juce::String targetText (int k) const;

    TridentProcessor& processor_;
    theme::Palette palette_;
    theme::KnobStyle knobStyle_;
    std::unique_ptr<SlotOverlay> overlay_;

    nova::Panel *ioPanel_ = nullptr, *navPanel_ = nullptr, *crossPanel_ = nullptr, *assignPanel_ = nullptr;
    nova::KnobCard *inKnob_ = nullptr, *outKnob_ = nullptr;
    nova::Text* latencyText_ = nullptr;
    CrossView* cross_ = nullptr;
    nova::Pill *bandsPill_ = nullptr, *slopePill_ = nullptr, *f1Pill_ = nullptr, *f2Pill_ = nullptr;
    nova::Pill* modePills_[3] {};
    struct PathUi
    {
        nova::Panel* panel = nullptr;
        nova::Pill *source = nullptr, *mute = nullptr, *solo = nullptr, *invert = nullptr;
        nova::KnobCard* gain = nullptr;
        nova::VMeter* meter = nullptr;
        nova::Text* latency = nullptr;
        struct SlotUi { nova::Pill *name = nullptr, *bypass = nullptr, *menu = nullptr; juce::String shown; } slots[kSlotsPerPath];
    } paths_[kPaths];
    struct AssignUi { nova::Text* text = nullptr; nova::Pill* clear = nullptr; juce::String shown; } assigns_[kAssignables];
    nova::Text* assignNote_ = nullptr;
};

// Graphe du séparateur : la réponse de chaque bande (grave, médium, aigu) sur l'échelle des fréquences, avec les coupures déplaçables. Glisser une coupure règle les deux
// bandes voisines à la fois ; Maj déplace les deux coupures ensemble.
class NovaTridentEditor::CrossView : public juce::Component
{
public:
    CrossView (const nova::Skin& s, ParamHost& host) : skin_ (s), host_ (host) {}

    void update()
    {
        const auto& ps = host_.parameters();
        const std::uint32_t v = ps.version (pid::bands) * 31u + ps.version (pid::xover1) * 17u + ps.version (pid::xover2) * 13u + ps.version (pid::slope) * 7u;
        if (v != seen_ || dragging_ != shownDragging_) { seen_ = v; shownDragging_ = dragging_; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (skin_.colour (Token::BgApp));
        g.fillRoundedRectangle (b, skin_.px (6.0f));
        const auto plot = plotArea();
        // grille : fréquences en échelle logarithmique, niveaux en dB
        g.setFont (skin_.font (9.0f));
        for (const double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xOf (f);
            g.setColour (skin_.colour (Token::Grid, 0.6f));
            g.fillRect (x - 0.5f, plot.getY(), std::max (1.0f, skin_.px (1.0f)), plot.getHeight());
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (f >= 1000.0 ? juce::String (static_cast<int> (f / 1000.0)) + "k" : juce::String (static_cast<int> (f)), juce::Rectangle<float> (x - skin_.px (16.0f), plot.getBottom() + skin_.px (2.0f), skin_.px (32.0f), skin_.px (12.0f)), juce::Justification::centred);
        }
        for (const int db : { 0, -6, -12, -24, -36, -48 })
        {
            const float y = yOf (static_cast<double> (db));
            g.setColour (skin_.colour (Token::Grid, db == 0 ? 0.9f : 0.5f));
            g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), std::max (1.0f, skin_.px (1.0f)));
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX() - skin_.px (30.0f), y - skin_.px (6.0f), skin_.px (24.0f), skin_.px (12.0f)), juce::Justification::centredRight);
        }
        // courbes des bandes
        Crossover x;
        const double fs = 48000.0;
        x.prepare (fs);
        const bool three = host_.parameters().get (pid::bands) >= 0.5;
        x.setBands (three ? 3 : 2);
        x.setSlope (slope());
        x.setFrequencies (f1(), f2());
        const Token tokens[3] = { Token::Accent, Token::Accent2, Token::Pink };
        for (int band = 0; band < 3; ++band)
        {
            if (! three && band == 1) continue;
            juce::Path line;
            const int n = static_cast<int> (plot.getWidth() / 2.0f);
            for (int i = 0; i <= n; ++i)
            {
                const double f = xToHz (static_cast<double> (i) / n);
                const double m = x.bandMagnitudes (f)[static_cast<std::size_t> (band)];
                const float px = plot.getX() + plot.getWidth() * static_cast<float> (i) / static_cast<float> (n);
                const float py = yOf (20.0 * std::log10 (std::max (m, 1.0e-4)));
                if (i == 0) line.startNewSubPath (px, py); else line.lineTo (px, py);
            }
            juce::Path fill (line);
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.lineTo (plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setColour (skin_.colour (tokens[band], 0.15f));
            g.fillPath (fill);
            g.setColour (skin_.colour (tokens[band]));
            g.strokePath (line, juce::PathStrokeType (std::max (1.5f, skin_.px (2.0f))));
        }
        // coupures
        for (int c = 0; c < (three ? 2 : 1); ++c)
        {
            const double f = c == 0 ? f1() : f2();
            const float px = xOf (f);
            const bool hot = dragged_ == c;
            g.setColour (skin_.colour (Token::Text, hot ? 0.9f : 0.5f));
            g.fillRect (px - 0.5f, plot.getY(), std::max (1.0f, skin_.px (1.0f)), plot.getHeight());
            const float r = skin_.px (hot ? 7.0f : 5.5f);
            g.setColour (skin_.colour (Token::Hring));
            g.fillEllipse (px - r - 2.0f, plot.getY() - r - 2.0f + skin_.px (8.0f), 2 * r + 4.0f, 2 * r + 4.0f);
            g.setColour (skin_.colour (Token::Handle));
            g.fillEllipse (px - r, plot.getY() - r + skin_.px (8.0f), 2 * r, 2 * r);
            g.setColour (skin_.colour (Token::Text));
            g.setFont (skin_.font (11.0f, true));
            const auto text = f >= 1000.0 ? juce::String (f / 1000.0, 2) + " kHz" : juce::String (static_cast<int> (std::lround (f))) + " Hz";
            g.drawText (text, juce::Rectangle<float> (px + skin_.px (8.0f), plot.getY() + skin_.px (2.0f), skin_.px (80.0f), skin_.px (14.0f)), juce::Justification::centredLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const bool three = host_.parameters().get (pid::bands) >= 0.5;
        const float x1 = xOf (f1()), x2 = xOf (f2());
        dragged_ = -1;
        const float reach = skin_.px (12.0f);
        const float d1 = std::fabs (e.position.x - x1), d2 = three ? std::fabs (e.position.x - x2) : 1.0e9f;
        if (std::min (d1, d2) <= reach) dragged_ = d1 <= d2 ? 0 : 1;
        else dragged_ = three && std::fabs (e.position.x - x1) > std::fabs (e.position.x - x2) ? 1 : 0;   // un clic ailleurs : la coupure la plus proche vient sous la souris
        dragging_ = true;
        startRatio_ = f2() / f1();
        host_.uiBeginEdit (pid::xover1);
        host_.uiBeginEdit (pid::xover2);
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged_ < 0) return;
        const auto plot = plotArea();
        const double f = std::clamp (xToHz ((e.position.x - plot.getX()) / plot.getWidth()), 20.0, 20000.0);
        const bool three = host_.parameters().get (pid::bands) >= 0.5;
        const auto setHz = [this] (ParamId id, double hz)
        {
            const auto* spec = host_.parameters().find (id);
            host_.uiSetNormalised (id, spec->toNormalized (std::clamp (hz, 20.0, 20000.0)));
        };
        if (e.mods.isShiftDown() && three)   // les deux coupures ensemble, à rapport constant
        {
            const double base = dragged_ == 0 ? f : f / startRatio_;
            setHz (pid::xover1, base);
            setHz (pid::xover2, base * startRatio_);
        }
        else if (dragged_ == 0)
        {
            setHz (pid::xover1, three ? std::min (f, f2() / 1.1) : f);
        }
        else
        {
            setHz (pid::xover2, std::max (f, f1() * 1.1));
        }
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        dragging_ = false;
        dragged_ = -1;
        host_.uiEndEdit (pid::xover2);
        host_.uiEndEdit (pid::xover1);
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        host_.uiResetToDefault (dragged_ == 1 ? pid::xover2 : pid::xover1);
    }

private:
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (skin_.px (34.0f), skin_.px (10.0f)).withTrimmedBottom (skin_.px (10.0f)); }
    float xOf (double hz) const { const auto p = plotArea(); return p.getX() + p.getWidth() * static_cast<float> (hzToX (std::clamp (hz, 20.0, 20000.0))); }
    float yOf (double db) const { const auto p = plotArea(); return p.getY() + p.getHeight() * static_cast<float> (std::clamp (-db, 0.0, 48.0) / 48.0) * 0.96f + p.getHeight() * 0.02f; }
    double f1() const { return host_.parameters().get (pid::xover1); }
    double f2() const { return std::max (host_.parameters().get (pid::xover2), f1() * 1.1); }
    int slope() const { const double s = host_.parameters().get (pid::slope); return s < 0.5 ? 12 : (s < 1.5 ? 24 : 48); }

    const nova::Skin& skin_;
    ParamHost& host_;
    std::uint32_t seen_ = 0xFFFFFFFFu;
    bool dragging_ = false, shownDragging_ = false;
    int dragged_ = -1;
    double startRatio_ = 12.0;
};

NovaTridentEditor::NovaTridentEditor (TridentProcessor& p, const theme::Palette& palette, theme::KnobStyle knob, const i18n::Translator& tr, Options options)
    : EditorBase (p, palette, knob, tr, std::move (options), U ("TRIDENT")), processor_ (p), palette_ (palette), knobStyle_ (knob)
{
    addPage (U ("ROUTAGE"), iconPath (0));
    addPage (U ("MACROS"), iconPath (1));
    addPage (tx ("nv.page_settings"), iconPath (2));
    auto& host = p;

    ioPanel_ = add<nova::Panel> (-1, skin());
    ioPanel_->setTitle (U ("NIVEAUX"));
    inKnob_ = add<nova::KnobCard> (-1, skin(), host, pid::inputGain, U ("ENTRÉE"), modalOpener(), false);
    outKnob_ = add<nova::KnobCard> (-1, skin(), host, pid::outputGain, U ("SORTIE"), modalOpener(), true);
    inKnob_->setPlain (true);
    outKnob_->setPlain (true);
    inKnob_->setCompact (true);
    outKnob_->setCompact (true);
    latencyText_ = add<nova::Text> (-1, skin(), Token::TextDim2, 11.0f);
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();

    // page ROUTAGE : le séparateur
    crossPanel_ = add<nova::Panel> (0, skin());
    crossPanel_->setTitle (U ("SÉPARATEUR DE FRÉQUENCES"));
    cross_ = add<CrossView> (0, skin(), host);
    bandsPill_ = add<nova::Pill> (0, skin());
    bandsPill_->bindChoice (host, pid::bands, U ("Bandes"), { U ("2"), U ("3") });
    slopePill_ = add<nova::Pill> (0, skin());
    slopePill_->bindChoice (host, pid::slope, U ("Pente"), { U ("12 dB/oct"), U ("24 dB/oct"), U ("48 dB/oct") });
    f1Pill_ = add<nova::Pill> (0, skin());
    f1Pill_->bindValue (host, pid::xover1, U ("Coupure 1"));
    f1Pill_->onClick = [this] { openModal (pid::xover1, uistate::Terminal::Pointer); };
    f2Pill_ = add<nova::Pill> (0, skin());
    f2Pill_->bindValue (host, pid::xover2, U ("Coupure 2"));
    f2Pill_->onClick = [this] { openModal (pid::xover2, uistate::Terminal::Pointer); };
    const char* modeNames[3] = { "Parallèle", "2 bandes + complet", "3 bandes" };
    for (int m = 0; m < 3; ++m)
    {
        modePills_[m] = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
        modePills_[m]->setText (U (modeNames[m]));
        modePills_[m]->onClick = [this, m] { applyRouting (m); };
    }
    for (int i = 0; i < kPaths; ++i)
    {
        auto& u = paths_[i];
        u.panel = add<nova::Panel> (0, skin());
        u.panel->setTitle (U ("CHEMIN ") + juce::String (i + 1));
        u.source = add<nova::Pill> (0, skin());
        u.source->bindChoice (host, pid::path (i, pid::source), U ("Source"), { U ("Complet"), U ("Grave"), U ("Médium"), U ("Aigu") });
        u.mute = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
        u.mute->bindToggle (host, pid::path (i, pid::mute), U ("Mute"));
        u.solo = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
        u.solo->bindToggle (host, pid::path (i, pid::solo), U ("Solo"));
        u.invert = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
        u.invert->bindToggle (host, pid::path (i, pid::invert), U ("Polarité"));
        u.gain = add<nova::KnobCard> (0, skin(), host, pid::path (i, pid::gain), U ("VOLUME"), modalOpener(), false);
        u.gain->setPlain (true);
        u.gain->setCompact (true);
        u.meter = add<nova::VMeter> (0, skin(), juce::String(), nova::VMeter::Kind::Level, false);
        u.meter->setModeVisible (false);
        u.latency = add<nova::Text> (0, skin(), Token::TextDim2, 11.0f);
        for (int s = 0; s < kSlotsPerPath; ++s)
        {
            auto& su = u.slots[s];
            su.name = add<nova::Pill> (0, skin());
            su.name->onClick = [this, i, s]
            {
                if (processor_.trident().effect (i, s) != nullptr) openSlot (i, s);
                else showEffectMenu (i, s, paths_[i].slots[s].name);
            };
            su.bypass = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
            su.bypass->setText (U ("Byp"));
            su.bypass->onClick = [this, i, s]
            {
                auto fx = processor_.trident().effect (i, s);
                if (fx == nullptr) return;
                auto& ps = fx->runner.params();
                const bool on = ps.get (stock::bypass) < 0.5;
                processor_.undo().setParameter (ps, stock::bypass, on ? 1.0 : 0.0);
                processor_.notifyStateChanged();
            };
            su.menu = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
            su.menu->setText (U ("..."));
            su.menu->onClick = [this, i, s] { showEffectMenu (i, s, paths_[i].slots[s].menu); };
        }
    }

    // page MACROS : les 32 paramètres assignables
    assignPanel_ = add<nova::Panel> (1, skin());
    assignPanel_->setTitle (U ("MACROS : 32 PARAMÈTRES ASSIGNABLES"));
    assignNote_ = add<nova::Text> (1, skin(), Token::TextDim2, 11.0f);
    assignNote_->setText (U ("Clic droit sur un potard (de Trident ou d'un effet) : « Assigner à une macro ». Chaque macro est un paramètre que l'hôte peut automatiser."));
    for (int k = 0; k < kAssignables; ++k)
    {
        assigns_[k].text = add<nova::Text> (1, skin(), Token::TextDim, 12.0f);
        assigns_[k].clear = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
        assigns_[k].clear->setText (U ("Retirer"));
        assigns_[k].clear->onClick = [this, k] { processor_.trident().unassign (k); processor_.notifyStateChanged(); };
    }
    buildSettingsPage (2);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaTridentEditor::~NovaTridentEditor() = default;

juce::String NovaTridentEditor::effectName (int path, int slot) const
{
    const auto fx = processor_.trident().effect (path, slot);
    if (fx == nullptr) return {};
    return tx (("module." + fx->type).c_str());
}

juce::String NovaTridentEditor::targetText (int k) const
{
    const auto t = processor_.trident().assignment (k);
    if (! t.active) return U ("-");
    if (t.path < 0)
    {
        const auto* spec = processor_.trident().params().find (t.param);
        return U ("Trident  -  ") + (spec != nullptr ? juce::String::fromUTF8 (spec->name.c_str()) : juce::String());
    }
    const auto fx = processor_.trident().effect (t.path, t.slot);
    const auto* spec = fx != nullptr ? fx->runner.params().find (t.param) : nullptr;
    return U ("Chemin ") + juce::String (t.path + 1) + "." + juce::String (t.slot + 1) + "  " + effectName (t.path, t.slot) + "  -  " + (spec != nullptr ? juce::String::fromUTF8 (spec->name.c_str()) : juce::String());
}

// Préréglages de routage : parallèle (complet x3), deux bandes + un complet, trois bandes.
void NovaTridentEditor::applyRouting (int mode)
{
    const int bands = mode == 2 ? 1 : (mode == 1 ? 0 : -1);
    const int sources[3][3] = { { 0, 0, 0 }, { 1, 3, 0 }, { 1, 2, 3 } };
    auto set = [this] (ParamId id, double plain)
    {
        const auto* spec = processor_.parameters().find (id);
        processor_.uiBeginEdit (id);
        processor_.uiSetNormalised (id, spec->toNormalized (plain));
        processor_.uiEndEdit (id);
    };
    if (bands >= 0) set (pid::bands, bands);
    for (int p = 0; p < kPaths; ++p) set (pid::path (p, pid::source), sources[mode][p]);
}

void NovaTridentEditor::showEffectMenu (int path, int slot, juce::Component* anchor)
{
    juce::PopupMenu menu;
    const bool filled = processor_.trident().effect (path, slot) != nullptr;
    int id = 1;
    for (const auto& m : availableModules()) menu.addItem (id++, tx (std::string (m.nameKey).c_str()), true, filled && processor_.trident().effect (path, slot)->type == m.typeId);
    if (filled)
    {
        menu.addSeparator();
        menu.addItem (1000, U ("Retirer l'effet"));
    }
    menu.setLookAndFeel (&anchor->getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    juce::Component::SafePointer<NovaTridentEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [safe, path, slot] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1000) { safe->processor_.setEffect (path, slot, {}); return; }
        const auto& mods = availableModules();
        if (r >= 1 && r <= static_cast<int> (mods.size())) safe->processor_.setEffect (path, slot, std::string (mods[static_cast<std::size_t> (r) - 1].typeId));
    });
}

void NovaTridentEditor::openSlot (int path, int slot)
{
    auto ov = std::make_unique<SlotOverlay> (processor_, path, slot, palette_, translator(), knobStyle_, [safe = juce::Component::SafePointer<NovaTridentEditor> (this)]
    {
        if (safe != nullptr) juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeOverlay(); });   // jamais depuis son propre bouton
    });
    if (! ov->valid()) return;
    overlay_ = std::move (ov);
    addAndMakeVisible (*overlay_);
    overlay_->setBounds (getLocalBounds());
    overlay_->grabKeyboardFocus();
}

void NovaTridentEditor::closeOverlay()
{
    overlay_.reset();
    grabKeyboardFocus();
}

void NovaTridentEditor::layout()
{
    ioPanel_->setBounds (R (14, 72, 176, 250));
    inKnob_->setBounds (R (20, 100, 84, 150));
    outKnob_->setBounds (R (100, 100, 84, 150));
    latencyText_->setBounds (R (24, 262, 160, 40));
    navPanel_->setBounds (R (14, 542, 176, 168));
    layoutNav (542, 168);

    // page ROUTAGE
    crossPanel_->setBounds (R (204, 72, 942, 240));
    cross_->setBounds (R (214, 100, 640, 204));
    bandsPill_->setBounds (R (872, 104, 262, 32));
    slopePill_->setBounds (R (872, 144, 262, 32));
    f1Pill_->setBounds (R (872, 184, 262, 32));
    f2Pill_->setBounds (R (872, 224, 262, 32));
    for (int m = 0; m < 3; ++m) modePills_[m]->setBounds (R (872.0f + static_cast<float> (m) * 88.0f, 270, 84, 30));
    const float colW = (942.0f - 24.0f) / 3.0f;
    for (int i = 0; i < kPaths; ++i)
    {
        const float x = 204.0f + static_cast<float> (i) * (colW + 12.0f);
        auto& u = paths_[i];
        u.panel->setBounds (R (x, 324, colW, 386));
        u.source->setBounds (R (x + 8, 358, colW - 16, 30));
        const float w3 = (colW - 16.0f - 8.0f) / 3.0f;
        u.mute->setBounds (R (x + 8, 396, w3, 28));
        u.solo->setBounds (R (x + 8 + w3 + 4, 396, w3, 28));
        u.invert->setBounds (R (x + 8 + 2 * (w3 + 4), 396, w3, 28));
        u.gain->setBounds (R (x + 8, 430, 110, 130));
        u.meter->setBounds (R (x + 130, 434, 42, 122));
        u.latency->setBounds (R (x + 184, 440, colW - 192, 40));
        for (int s = 0; s < kSlotsPerPath; ++s)
        {
            const float y = 574.0f + static_cast<float> (s) * 33.0f - 0.0f;
            const float yy = 566.0f + static_cast<float> (s) * 34.0f;
            (void) y;
            u.slots[s].name->setBounds (R (x + 8, yy, colW - 16 - 100, 30));
            u.slots[s].bypass->setBounds (R (x + colW - 8 - 96, yy, 46, 30));
            u.slots[s].menu->setBounds (R (x + colW - 8 - 46, yy, 46, 30));
        }
    }

    // page MACROS
    assignPanel_->setBounds (R (204, 72, 942, 638));
    assignNote_->setBounds (R (220, 104, 910, 20));
    for (int k = 0; k < kAssignables; ++k)
    {
        const int col = k / 16, row = k % 16;
        const float x = 220.0f + static_cast<float> (col) * 458.0f, y = 134.0f + static_cast<float> (row) * 34.0f;
        assigns_[k].text->setBounds (R (x, y + 4, 340, 22));
        assigns_[k].clear->setBounds (R (x + 352, y, 90, 28));
    }
}

void NovaTridentEditor::tick()
{
    auto& t = processor_.trident();
    cross_->update();
    const int lat = t.latencySamples();
    const double sr = processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0;
    latencyText_->setText (U ("Latence : ") + juce::String (lat) + U (" éch.  (") + juce::String (1000.0 * lat / sr, 1) + " ms)");
    const bool three = value (pid::bands) >= 0.5;
    f2Pill_->setDim (! three);
    for (int i = 0; i < kPaths; ++i)
    {
        auto& u = paths_[i];
        const float peak = t.pathPeak (i);
        u.meter->update (dbOf (peak), dbOf (peak));
        u.latency->setText (U ("Latence du chemin : ") + juce::String (t.pathLatency (i)) + U (" éch."));
        for (int s = 0; s < kSlotsPerPath; ++s)
        {
            auto& su = u.slots[s];
            const auto fx = t.effect (i, s);
            const auto text = fx == nullptr ? U ("+ effet") : effectName (i, s);
            if (text != su.shown) { su.shown = text; su.name->setText (text); }
            su.name->setDim (fx == nullptr);
            su.bypass->setVisible (fx != nullptr);
            su.bypass->setOn (fx != nullptr && fx->runner.params().get (stock::bypass) >= 0.5);
        }
    }
    if (currentPage() == 1)
        for (int k = 0; k < kAssignables; ++k)
        {
            const auto text = juce::String (k + 1) + U ("   ") + targetText (k);
            if (text != assigns_[k].shown) { assigns_[k].shown = text; assigns_[k].text->setText (text); }
            assigns_[k].clear->setVisible (t.assignment (k).active);
        }
}

// ================================================================================================ fenêtre du plugin

TridentPluginEditor::TridentPluginEditor (TridentProcessor& p) : juce::AudioProcessorEditor (p), processor_ (p), tr_ (p.settings().language)
{
    buildContent();
    setWantsKeyboardFocus (true);
    nova::configureStretchConstrainer (constrainer_, nova::EditorBase::designWidth, nova::EditorBase::designHeight, nova::EditorBase::minScale, nova::EditorBase::maxScale, 4096);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (nova::EditorBase::designWidth, nova::EditorBase::designHeight);
}

void TridentPluginEditor::buildContent()
{
    auto& p = processor_;
    tr_ = i18n::Translator (p.settings().language);
    const auto palette = makePalette (p.settings());
    const auto& def = theme::definition (theme::themeFromString (p.settings().theme).value_or (theme::ThemeId::Dark));
    nova::EditorBase::Options opt;
    opt.brand = true;
    opt.moduleName = "trident";
    opt.stretchAuto = true;
    opt.noPresets = true;
    opt.noAB = true;
    opt.noBypass = true;
    opt.sampleRate = [&p] { return p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0; };
    opt.getSettings = [&p] { return p.settings(); };
    opt.applySettings = [&p, safe = juce::Component::SafePointer<TridentPluginEditor> (this)] (const settings::GlobalSettings& s)
    {
        p.applySettings (s);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->buildContent(); safe->resized(); } });
    };
    content_.reset();
    content_ = std::make_unique<NovaTridentEditor> (p, palette, def.knob, tr_, opt);
    addAndMakeVisible (*content_);
    content_->setBounds (getLocalBounds());
}

void TridentPluginEditor::resized() { content_->setBounds (getLocalBounds()); }
void TridentPluginEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
bool TridentPluginEditor::keyPressed (const juce::KeyPress& k) { return content_ != nullptr && content_->keyPressed (k); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TridentProcessor(); }
