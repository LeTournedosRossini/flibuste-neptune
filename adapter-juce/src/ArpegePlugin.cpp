#include "ArpegePlugin.h"

#include <algorithm>
#include <cmath>

using namespace neptune::core;
namespace arp = neptune::core::arp;

namespace
{
constexpr int kDesignW = 880, kDesignH = 230;

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
const theme::ThemeDefinition& themeOf (const settings::GlobalSettings& st)
{
    return theme::definition (theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark));
}

const char* const kPatternNames[] = { "Montant", "Descendant", "Aller-retour", "Aller-retour (bouts doublés)", "Aléatoire", "Ordre de saisie", "Accord" };
const char* const kRateNames[] = { "1/4", "1/8", "1/8 triolet", "1/16", "1/16 triolet", "1/32", "1/32 triolet", "1/2" };
const char* const kVelNames[] = { "Jouée", "Fixe" };
}

const std::array<double, 8>& ArpegeProcessor::rateBeats()
{
    static const std::array<double, 8> v = { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 2.0 };
    return v;
}

// ================================================================================================ processeur

ArpegeProcessor::ArpegeProcessor() : juce::AudioProcessor (BusesProperties())
{
    juce::StringArray patterns, rates, vels;
    for (auto* s : kPatternNames) patterns.add (juce::String::fromUTF8 (s));
    for (auto* s : kRateNames) rates.add (juce::String::fromUTF8 (s));
    for (auto* s : kVelNames) vels.add (juce::String::fromUTF8 (s));
    addParameter (pattern = new juce::AudioParameterChoice (juce::ParameterID { "pattern", 1 }, "Motif", patterns, 0));
    addParameter (octaves = new juce::AudioParameterInt (juce::ParameterID { "octaves", 1 }, "Octaves", 1, 4, 1));
    addParameter (rate = new juce::AudioParameterChoice (juce::ParameterID { "rate", 1 }, "Pas", rates, 3));
    addParameter (gate = new juce::AudioParameterFloat (juce::ParameterID { "gate", 1 }, "Duree de note", juce::NormalisableRange<float> (5.0f, 100.0f, 1.0f), 50.0f));
    addParameter (swing = new juce::AudioParameterFloat (juce::ParameterID { "swing", 1 }, "Swing", juce::NormalisableRange<float> (0.0f, 75.0f, 1.0f), 0.0f));
    addParameter (hold = new juce::AudioParameterBool (juce::ParameterID { "hold", 1 }, "Maintien", false));
    addParameter (velMode = new juce::AudioParameterChoice (juce::ParameterID { "velMode", 1 }, "Velocite", vels, 0));
    addParameter (velocity = new juce::AudioParameterInt (juce::ParameterID { "velocity", 1 }, "Velocite fixe", 1, 127, 100));
    addParameter (accentEvery = new juce::AudioParameterInt (juce::ParameterID { "accentEvery", 1 }, "Accent un pas sur", 0, 16, 0));
    addParameter (accentBoost = new juce::AudioParameterInt (juce::ParameterID { "accentBoost", 1 }, "Force de l'accent", 0, 60, 20));
    addParameter (transpose = new juce::AudioParameterInt (juce::ParameterID { "transpose", 1 }, "Transposition", -24, 24, 0));

    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    in_.reserve (1024);
    out_.resize (4096);
}

arp::Params ArpegeProcessor::currentParams() const
{
    arp::Params p;
    p.pattern = static_cast<arp::Pattern> (pattern->getIndex());
    p.octaves = octaves->get();
    p.stepBeats = rateBeats()[static_cast<size_t> (rate->getIndex())];
    p.gate = gate->get() / 100.0;
    p.swing = swing->get() / 100.0;
    p.hold = hold->get();
    p.fixedVelocity = velMode->getIndex() == 1;
    p.velocity = velocity->get();
    p.accentEvery = accentEvery->get();
    p.accentBoost = accentBoost->get();
    p.transpose = transpose->get();
    return p;
}

void ArpegeProcessor::prepareToPlay (double sampleRate, int)
{
    arp_.setSampleRate (sampleRate);
    arp_.reset();
}

void ArpegeProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    audio.clear();
    arp_.setParams (currentParams());

    arp::Transport tr;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            tr.playing = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition())
            {
                tr.hasPosition = true;
                tr.ppq = *ppq;
            }
            if (const auto bpm = pos->getBpm()) tr.bpm = *bpm;
        }
    playing_.store (tr.playing);

    in_.clear();
    scratch_.clear();
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn() || m.isNoteOff())
            in_.push_back ({ meta.samplePosition, m.isNoteOn(), static_cast<std::uint8_t> (m.getNoteNumber()), static_cast<std::uint8_t> (m.getVelocity()) });
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            arp_.reset();
        else
            scratch_.addEvent (m, meta.samplePosition);   // le reste passe sans changement
    }
    std::stable_sort (in_.begin(), in_.end(), [] (const arp::NoteIn& a, const arp::NoteIn& b) { return a.offset < b.offset; });

    const int n = arp_.process (in_.data(), static_cast<int> (in_.size()), tr, audio.getNumSamples() > 0 ? audio.getNumSamples() : 0, out_.data(), static_cast<int> (out_.size()));
    for (int i = 0; i < n; ++i)
    {
        const auto& e = out_[static_cast<size_t> (i)];
        scratch_.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, static_cast<juce::uint8> (e.velocity))
                                : juce::MidiMessage::noteOff (1, e.note), e.offset);
    }
    midi.swapWith (scratch_);
    held_.store (arp_.heldCount());
}

void ArpegeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("ARPEGE");
    xml.setAttribute ("version", 1);
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p)) xml.setAttribute (r->getParameterID(), r->convertFrom0to1 (r->getValue()));
    copyXmlToBinary (xml, dest);
}

void ArpegeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("ARPEGE") && xml->getIntAttribute ("version") == 1)
            for (auto* p : getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (xml->hasAttribute (r->getParameterID())) r->setValueNotifyingHost (r->convertTo0to1 (static_cast<float> (xml->getDoubleAttribute (r->getParameterID()))));
}

juce::AudioProcessorEditor* ArpegeProcessor::createEditor() { return new ArpegeEditor (*this); }

// ================================================================================================ interface

// Pastille liée à un paramètre : glisser = régler, clic = changer de choix (ou basculer), double-clic = saisir la valeur.
class ArpegeEditor::ParamPill : public nova::Pill
{
public:
    ParamPill (const nova::Skin& s, juce::RangedAudioParameter& p, juce::String label)
        : nova::Pill (s, nova::Pill::Look::Small), param_ (p), label_ (std::move (label))
    {
        refresh();
    }

    void refresh() override
    {
        const auto text = label_ + "  " + param_.getCurrentValueAsText();
        if (text != shown_) { shown_ = text; setText (text); }
        if (dynamic_cast<juce::AudioParameterBool*> (&param_) != nullptr) setOn (param_.getValue() > 0.5f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartY_ = e.position.y;
        startValue_ = param_.getValue();
        moved_ = false;
        if (! isChoiceLike()) param_.beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (isChoiceLike()) return;
        const float dy = dragStartY_ - e.position.y;
        if (std::abs (dy) > 2.0f) moved_ = true;
        const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
        param_.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, startValue_ + fine * dy / 160.0f));
        refresh();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isChoiceLike())
        {
            if (! getLocalBounds().contains (e.getPosition())) return;
            const int n = param_.getNumSteps();
            const int cur = juce::roundToInt (param_.getValue() * (float) (n - 1));
            const int next = e.mods.isRightButtonDown() || e.mods.isShiftDown() ? (cur + n - 1) % n : (cur + 1) % n;
            param_.beginChangeGesture();
            param_.setValueNotifyingHost ((float) next / (float) (n - 1));
            param_.endChangeGesture();
            refresh();
        }
        else
            param_.endChangeGesture();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (isChoiceLike()) return;
        auto* box = new juce::AlertWindow (label_, juce::String::fromUTF8 ("Valeur :"), juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("v", param_.getText (param_.getValue(), 16).retainCharacters ("-0123456789."));
        box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Annuler", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ParamPill> self (this);
        box->enterModalState (true, juce::ModalCallbackFunction::create ([self, box] (int r)
        {
            if (r == 1 && self != nullptr)
            {
                const float v = box->getTextEditorContents ("v").getFloatValue();
                self->param_.beginChangeGesture();
                self->param_.setValueNotifyingHost (self->param_.convertTo0to1 (v));
                self->param_.endChangeGesture();
                self->refresh();
            }
        }), true);
    }

private:
    bool isChoiceLike() const
    {
        return dynamic_cast<juce::AudioParameterChoice*> (&param_) != nullptr || dynamic_cast<juce::AudioParameterBool*> (&param_) != nullptr;
    }

    juce::RangedAudioParameter& param_;
    juce::String label_, shown_;
    float dragStartY_ = 0.0f, startValue_ = 0.0f;
    bool moved_ = false;
};

ArpegeEditor::ArpegeEditor (ArpegeProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p), skin_ (makePalette (p.settings()), themeOf (p.settings()).knob, themeOf (p.settings()).id)
{
    const auto add = [this] (juce::RangedAudioParameter* prm, const char* label)
    {
        pills_.push_back (std::make_unique<ParamPill> (skin_, *prm, juce::String::fromUTF8 (label)));
        addAndMakeVisible (*pills_.back());
    };
    add (proc_.pattern, "Motif");
    add (proc_.octaves, "Octaves");
    add (proc_.rate, "Pas");
    add (proc_.gate, "Durée %");
    add (proc_.swing, "Swing %");
    add (proc_.hold, "Maintien");
    add (proc_.velMode, "Vélocité");
    add (proc_.velocity, "Fixe");
    add (proc_.accentEvery, "Accent 1 pas sur");
    add (proc_.accentBoost, "Force");
    add (proc_.transpose, "Transposition");

    nova::configureConstrainer (constrainer_, kDesignW, kDesignH, 0.6f, 3.0f);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (kDesignW, kDesignH);
    startTimerHz (15);
}

ArpegeEditor::~ArpegeEditor() { stopTimer(); }

void ArpegeEditor::resized()
{
    skin_.setScale (nova::fitScale (getWidth(), getHeight(), kDesignW, kDesignH, 0.6f, 3.0f));
    const auto px = [this] (float v) { return skin_.ipx (v); };
    // deux rangées : le jeu (motif, octaves, pas, durée, swing) puis la dynamique et la hauteur
    const float w[] = { 260, 110, 150, 120, 120, 120, 120, 100, 190, 100, 170 };
    const int rowOf[] = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 };
    int x[2] = { px (16.0f), px (16.0f) };
    const int y0 = px (66.0f), h = px (30.0f), gap = px (46.0f);
    for (size_t i = 0; i < pills_.size(); ++i)
    {
        const int r = rowOf[i];
        pills_[i]->setBounds (x[r], y0 + r * gap, px (w[i]), h);
        x[r] += px (w[i] + 8.0f);
    }
}

void ArpegeEditor::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (theme::Token::BgApp));
    nova::paintHeaderBar (g, skin_, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), skin_.px (46.0f)), juce::String::fromUTF8 ("Arpège"), juce::String::fromUTF8 ("arpégiateur"));
    g.setFont (skin_.font (13.0f));
    g.setColour (skin_.colour (theme::Token::TextDim));
    const auto text = juce::String::fromUTF8 ("Notes tenues : ") + juce::String (proc_.heldCount()) + "   ·   "
                      + (proc_.hostPlaying() ? juce::String::fromUTF8 ("calé sur la lecture de l'hôte") : juce::String::fromUTF8 ("lecture arrêtée : horloge interne"))
                      + juce::String::fromUTF8 ("   ·   glisser : régler  ·  clic : changer  ·  double-clic : saisir");
    g.drawText (text, juce::Rectangle<float> (skin_.px (16.0f), static_cast<float> (getHeight()) - skin_.px (34.0f), static_cast<float> (getWidth()) - skin_.px (32.0f), skin_.px (24.0f)).toNearestInt(),
                juce::Justification::centredLeft, true);
}

void ArpegeEditor::timerCallback()
{
    for (auto& p : pills_) p->refresh();
    repaint (0, getHeight() - skin_.ipx (40.0f), getWidth(), skin_.ipx (40.0f));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArpegeProcessor(); }
