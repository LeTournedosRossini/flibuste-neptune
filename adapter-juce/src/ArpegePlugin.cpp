#include "ArpegePlugin.h"

#include <algorithm>
#include <cmath>

using namespace neptune::core;
namespace arp = neptune::core::arp;

namespace
{
constexpr int kDesignW = 880, kDesignH = 560;

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

namespace
{
struct Fourni { const char* name; const char* json; };
const Fourni kFournis[] = {
    { "Arpège montant", R"({ "id": "montant", "name": "Arpège montant", "length": [1,2], "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["3"] },
        { "at": [2,8], "dur": [1,8], "parts": ["5"] }, { "at": [3,8], "dur": [1,8], "parts": ["5+1"] } ] })" },
    { "Alberti", R"({ "id": "alberti", "name": "Alberti", "length": [1,2], "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["5"] },
        { "at": [2,8], "dur": [1,8], "parts": ["3"] }, { "at": [3,8], "dur": [1,8], "parts": ["5"] } ] })" },
    { "Triolets montants", R"({ "id": "triolets", "name": "Triolets montants", "length": [1,4], "hits": [
        { "at": [0,12], "dur": [1,12], "parts": ["1"] }, { "at": [1,12], "dur": [1,12], "parts": ["3"] }, { "at": [2,12], "dur": [1,12], "parts": ["5"] } ] })" },
    { "Ballade", R"({ "id": "ballade", "name": "Ballade", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["B","C"] }, { "at": [1,4], "dur": [1,4], "parts": ["C"] },
        { "at": [2,4], "dur": [1,4], "parts": ["C"] }, { "at": [3,4], "dur": [1,4], "parts": ["C"] } ] })" },
    { "Valse", R"({ "id": "valse", "name": "Valse", "length": [3,4], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["B"] }, { "at": [1,4], "dur": [1,4], "parts": ["C"] }, { "at": [2,4], "dur": [1,4], "parts": ["C"] } ] })" },
    { "Basse et contretemps", R"({ "id": "contretemps", "name": "Basse et contretemps", "length": [1,1], "hits": [
        { "at": [0,8], "dur": [1,4], "parts": ["B"] }, { "at": [1,8], "dur": [1,8], "parts": ["C"] }, { "at": [3,8], "dur": [1,8], "parts": ["C"] },
        { "at": [4,8], "dur": [1,4], "parts": ["B"] }, { "at": [5,8], "dur": [1,8], "parts": ["C"] }, { "at": [7,8], "dur": [1,8], "parts": ["C"] } ] })" },
    { "Accord tenu", R"({ "id": "tenu", "name": "Accord tenu", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,1], "parts": ["C"] } ] })" },
};
constexpr int kNumFournis = (int) (sizeof (kFournis) / sizeof (kFournis[0]));
}

std::vector<neptune::core::pattern::Pattern> ArpegeProcessor::builtinPatterns()
{
    std::vector<pattern::Pattern> v;
    for (const auto& f : kFournis)
    {
        const auto r = pattern::fromJson (f.json);
        if (r.ok && ! r.patterns.empty()) v.push_back (r.patterns[0]);
    }
    return v;
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

    addParameter (slot = new juce::AudioParameterInt (juce::ParameterID { "slot", 1 }, "Motif", 0, kSlots, 0, juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? juce::String ("Auto") : juce::String (v); })));
    addParameter (absentMode = new juce::AudioParameterChoice (juce::ParameterID { "absent", 1 }, "Degre absent", juce::StringArray { "Sauter", "Jouer" }, 0));

    {
        const auto b = builtinPatterns();
        for (size_t i = 0; i < b.size() && i < (size_t) kSlots; ++i) slots_[i] = std::make_shared<const pattern::Pattern> (b[i]);
    }
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

bool ArpegeProcessor::slotUsed (int i) const
{
    const juce::SpinLock::ScopedLockType l (slotLock_);
    return i >= 0 && i < kSlots && slots_[(size_t) i] != nullptr;
}

pattern::Pattern ArpegeProcessor::slotPattern (int i) const
{
    const juce::SpinLock::ScopedLockType l (slotLock_);
    if (i >= 0 && i < kSlots && slots_[(size_t) i] != nullptr) return *slots_[(size_t) i];
    pattern::Pattern p;
    p.id = "motif" + std::to_string (i + 1);
    return p;
}

void ArpegeProcessor::setSlotPattern (int i, pattern::Pattern p)
{
    if (i < 0 || i >= kSlots) return;
    if (p.id.empty()) p.id = "motif" + std::to_string (i + 1);
    auto np = std::make_shared<const pattern::Pattern> (std::move (p));
    {
        const juce::SpinLock::ScopedLockType l (slotLock_);
        if (slots_[(size_t) i] != nullptr) retired_.push_back (slots_[(size_t) i]);
        if (retired_.size() > 64) retired_.erase (retired_.begin(), retired_.begin() + 32);
        slots_[(size_t) i] = std::move (np);
    }
    slotsVersion_.fetch_add (1);
}

void ArpegeProcessor::clearSlot (int i)
{
    if (i < 0 || i >= kSlots) return;
    {
        const juce::SpinLock::ScopedLockType l (slotLock_);
        if (slots_[(size_t) i] != nullptr) retired_.push_back (slots_[(size_t) i]);
        slots_[(size_t) i] = nullptr;
    }
    slotsVersion_.fetch_add (1);
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

    {
        // motif choisi : on ne change de motif que si le verrou est libre (sinon au bloc suivant)
        const int s = slot->get(), ver = slotsVersion_.load();
        if (s != activeSlot_ || ver != activeVersion_)
        {
            const juce::SpinLock::ScopedTryLockType l (slotLock_);
            if (l.isLocked())
            {
                active_ = s > 0 ? slots_[(size_t) s - 1] : nullptr;
                activeSlot_ = s;
                activeVersion_ = ver;
            }
        }
        arp_.setPattern (active_.get(), absentMode->getIndex() == 1 ? pattern::Absent::Play : pattern::Absent::Skip);
    }

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
            if (const auto ts = pos->getTimeSignature())
                if (ts->denominator > 0) tr.beatsPerBar = 4.0 * ts->numerator / ts->denominator;
            if (const auto bar = pos->getPpqPositionOfLastBarStart()) tr.barStartPpq = *bar;
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
    for (int i = 0; i < kSlots; ++i)
        if (slotUsed (i))
        {
            auto* e = xml.createNewChildElement ("SLOT");
            e->setAttribute ("n", i + 1);
            e->addTextElement (juce::String (pattern::toJson ({ slotPattern (i) }, "arpege")));
        }
    copyXmlToBinary (xml, dest);
}

void ArpegeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("ARPEGE") && xml->getIntAttribute ("version") == 1)
            for (auto* p : getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (xml->hasAttribute (r->getParameterID())) r->setValueNotifyingHost (r->convertTo0to1 (static_cast<float> (xml->getDoubleAttribute (r->getParameterID()))));
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("ARPEGE"))
        {
            for (int i = 0; i < kSlots; ++i) clearSlot (i);
            for (auto* e : xml->getChildWithTagNameIterator ("SLOT"))
            {
                const int n = e->getIntAttribute ("n");
                const auto r = pattern::fromJson (e->getAllSubText().toStdString());
                if (n >= 1 && n <= kSlots && r.ok && ! r.patterns.empty()) setSlotPattern (n - 1, r.patterns[0]);
            }
        }
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

// Matrice d'un motif : colonnes = cases de la grille sur la longueur du motif, lignes = degrés de l'accord (de bas en haut : basse, 1 à 7, puis les mêmes une
// octave plus haut). Un clic allume ou éteint une case (glisser : peindre) ; la bande du bas pose l'accent d'un coup.
namespace
{
using pattern::Frac;
using pattern::makeFrac;
const char* const kRowParts[] = { "5+1", "4+1", "3+1", "2+1", "1+1", "7", "6", "5", "4", "3", "2", "1", "B" };
constexpr int kNumRows = 13;
const Frac kGridFracs[] = { { 1, 8 }, { 1, 12 }, { 1, 16 }, { 1, 24 }, { 1, 32 } };
const char* const kGridNames[] = { "1/8", "1/12 (triolet)", "1/16", "1/24 (triolet)", "1/32" };
constexpr int kNumGrids = 5;
const Frac kLengths[] = { { 1, 4 }, { 1, 2 }, { 3, 4 }, { 1, 1 }, { 3, 2 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };
constexpr int kNumLengths = 8;

juce::String rowLabel (const juce::String& part)
{
    if (part == "B") return "B";
    return part.upToFirstOccurrenceOf ("+", false, false) + (part.contains ("+") ? juce::String::fromUTF8 ("\xE2\x86\x91") : juce::String());
}
}

class ArpegeEditor::MatrixView : public juce::Component
{
public:
    MatrixView (ArpegeProcessor& p, const nova::Skin& s) : proc_ (p), skin_ (s) {}

    int grid = 2;
    std::function<void()> onChange;

    int slotIndex() const { return proc_.slot->get() - 1; }
    int columns() const
    {
        const auto p = proc_.slotPattern (slotIndex());
        const auto g = kGridFracs[grid];
        return std::clamp ((int) ((p.length.n * g.d + p.length.d * g.n - 1) / (p.length.d * g.n)), 1, 96);
    }

    void paint (juce::Graphics& g) override
    {
        using theme::Token;
        const auto area = getLocalBounds().toFloat();
        skin_.fill (g, Token::PanelBg, area, skin_.px (8.0f));
        if (slotIndex() < 0)
        {
            g.setColour (skin_.colour (Token::TextDim));
            g.setFont (skin_.font (14.0f));
            g.drawFittedText (juce::String::fromUTF8 ("Motifs automatiques. Choisissez un emplacement de motif (1 à 16) pour dessiner le vôtre ici."), getLocalBounds().reduced (skin_.ipx (20.0f)), juce::Justification::centred, 2);
            return;
        }
        const auto pat = proc_.slotPattern (slotIndex());
        const auto gr = kGridFracs[grid];
        const int cols = columns();
        const auto geo = geometry (cols);
        // lignes
        for (int r = 0; r < kNumRows; ++r)
        {
            const auto row = juce::Rectangle<float> (geo.x0, geo.y0 + r * geo.rowH, geo.w, geo.rowH);
            if (r % 2 == 0) { g.setColour (skin_.colour (Token::BgCard, 0.45f)); g.fillRect (row); }
            g.setColour (skin_.colour (Token::TextDim));
            g.setFont (skin_.font (12.0f));
            g.drawText (rowLabel (kRowParts[r]), juce::Rectangle<float> (0.0f, row.getY(), geo.x0 - skin_.px (6.0f), row.getHeight()), juce::Justification::centredRight);
        }
        // traits de temps et de mesure
        for (int c = 0; c <= cols; ++c)
        {
            const bool beat = (gr.n * c * 4) % gr.d == 0;
            const bool bar = (gr.n * c) % (pat.barLength().n * 1) == 0 && pat.barLength().d == gr.d ? false : false;
            (void) bar;
            if (! beat) continue;
            g.setColour (skin_.colour (Token::Border, 0.8f));
            const float x = geo.x0 + c * geo.cellW;
            g.drawLine (x, geo.y0, x, geo.y0 + geo.rowH * kNumRows, skin_.px (1.0f));
        }
        // cases allumées (la longueur du coup est dessinée)
        for (const auto& h : pat.hits)
        {
            const double colF = (h.at.value()) / gr.value();
            if (std::abs (colF - std::round (colF)) > 1e-6) continue;   // coup hors de cette grille : visible dans une autre
            const int c = (int) std::round (colF);
            const float widthCells = (float) std::max (1.0, h.dur.value() / gr.value());
            for (const auto& part : h.parts)
            {
                juce::String t (part.text);
                if (t.startsWithChar ('L')) t = t.substring (1);
                for (int r = 0; r < kNumRows; ++r)
                    if (t == kRowParts[r])
                    {
                        const auto cell = juce::Rectangle<float> (geo.x0 + c * geo.cellW, geo.y0 + r * geo.rowH, widthCells * geo.cellW, geo.rowH).reduced (skin_.px (1.5f));
                        skin_.fillActive (g, cell, skin_.px (4.0f), 2.0f);
                        g.setColour (skin_.colour (h.accent ? Token::Pink : Token::Accent));
                        g.drawRoundedRectangle (cell, skin_.px (4.0f), skin_.px (1.5f));
                    }
            }
            // bande d'accent
            const auto acc = juce::Rectangle<float> (geo.x0 + c * geo.cellW, geo.accentY, geo.cellW, geo.accentH).reduced (skin_.px (2.0f));
            g.setColour (skin_.colour (h.accent ? Token::Pink : Token::TextDim2, h.accent ? 1.0f : 0.5f));
            g.fillRoundedRectangle (acc, skin_.px (3.0f));
        }
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (11.0f));
        g.drawText (juce::String::fromUTF8 ("accent"), juce::Rectangle<float> (0.0f, geo.accentY, geo.x0 - skin_.px (6.0f), geo.accentH), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (slotIndex() < 0) return;
        last_ = { -1, -1 };
        apply (e.position, true);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (slotIndex() >= 0 && painting_) apply (e.position, false); }
    void mouseUp (const juce::MouseEvent&) override { painting_ = false; }

private:
    struct Geo { float x0, y0, w, rowH, cellW, accentY, accentH; };
    Geo geometry (int cols) const
    {
        Geo g {};
        g.x0 = skin_.px (46.0f);
        g.y0 = skin_.px (6.0f);
        g.accentH = skin_.px (18.0f);
        g.w = (float) getWidth() - g.x0 - skin_.px (8.0f);
        g.rowH = ((float) getHeight() - g.y0 - g.accentH - skin_.px (14.0f)) / (float) kNumRows;
        g.cellW = g.w / (float) cols;
        g.accentY = g.y0 + g.rowH * kNumRows + skin_.px (4.0f);
        return g;
    }

    void apply (juce::Point<float> pos, bool first)
    {
        const int idx = slotIndex();
        const int cols = columns();
        const auto geo = geometry (cols);
        const int c = (int) std::floor ((pos.x - geo.x0) / geo.cellW);
        if (c < 0 || c >= cols) return;
        auto pat = proc_.slotPattern (idx);
        const auto gr = kGridFracs[grid];
        const auto at = makeFrac (gr.n * c, gr.d);
        if (pos.y >= geo.accentY)
        {
            if (! first) return;
            if (pattern::toggleAccent (pat, at)) {}
            proc_.setSlotPattern (idx, pat);
        }
        else
        {
            const int r = (int) std::floor ((pos.y - geo.y0) / geo.rowH);
            if (r < 0 || r >= kNumRows) return;
            if (last_.x == c && last_.y == r) return;
            last_ = { c, r };
            if (first)
            {
                paintOn_ = ! pattern::hasCell (pat, at, kRowParts[r]);
                painting_ = true;
            }
            if (pattern::hasCell (pat, at, kRowParts[r]) == paintOn_) return;
            pattern::toggleCell (pat, at, gr, kRowParts[r]);
            proc_.setSlotPattern (idx, pat);
        }
        repaint();
        if (onChange) onChange();
    }

    ArpegeProcessor& proc_;
    const nova::Skin& skin_;
    juce::Point<int> last_ { -1, -1 };
    bool painting_ = false, paintOn_ = true;
};

ArpegeEditor::ArpegeEditor (ArpegeProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p), skin_ (makePalette (p.settings()), themeOf (p.settings()).knob, themeOf (p.settings()).id)
{
    const auto add = [this] (juce::RangedAudioParameter* prm, const char* label)
    {
        pills_.push_back (std::make_unique<ParamPill> (skin_, *prm, juce::String::fromUTF8 (label)));
        addAndMakeVisible (*pills_.back());
    };
    add (proc_.pattern, "Motif auto");
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
    add (proc_.slot, "Motif n°");
    add (proc_.absentMode, "Degré absent");

    matrix_ = std::make_unique<MatrixView> (proc_, skin_);
    addAndMakeVisible (*matrix_);

    // actions : 0 grille, 1 durée, 2 mesures, 3 fourni, 4 charger, 5 importer, 6 exporter, 7 effacer
    for (int i = 0; i < 8; ++i)
    {
        actions_.push_back (std::make_unique<nova::Pill> (skin_, nova::Pill::Look::Small));
        addAndMakeVisible (*actions_.back());
    }
    const auto withPattern = [this] (const std::function<void (pattern::Pattern&)>& edit)
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        auto pat = proc_.slotPattern (idx);
        edit (pat);
        proc_.setSlotPattern (idx, pat);
        matrix_->repaint();
        refreshActions();
    };
    actions_[0]->onClick = [this] { matrix_->grid = (matrix_->grid + 1) % kNumGrids; matrix_->repaint(); refreshActions(); };
    actions_[1]->onClick = [this, withPattern]
    {
        withPattern ([] (pattern::Pattern& pt)
        {
            int cur = 0;
            for (int i = 0; i < kNumLengths; ++i) if (pattern::compare (pt.length, kLengths[i]) == 0) cur = i;
            pattern::setLengthAndBars (pt, kLengths[(cur + 1) % kNumLengths], pt.bars);
        });
    };
    actions_[2]->onClick = [withPattern] { withPattern ([] (pattern::Pattern& pt) { pattern::setLengthAndBars (pt, pt.length, pt.bars % 4 + 1); }); };
    actions_[3]->onClick = [this] { fournis_ = (fournis_ + 1) % (int) proc_.builtinPatterns().size(); refreshActions(); };
    actions_[4]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        const auto all = proc_.builtinPatterns();
        if (idx >= 0 && fournis_ < (int) all.size()) proc_.setSlotPattern (idx, all[(size_t) fournis_]);
        matrix_->repaint();
        refreshActions();
    };
    actions_[5]->onClick = [this]
    {
        if (proc_.slot->get() < 1) return;
        chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Importer un motif (Arpège ou Scorsaire)"), juce::File(), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const auto r = pattern::fromJson (f.loadFileAsString().toStdString());
            if (! r.ok || r.patterns.empty()) return;
            int idx = proc_.slot->get() - 1;
            for (const auto& pt : r.patterns)
            {
                if (idx >= ArpegeProcessor::kSlots) break;
                proc_.setSlotPattern (idx++, pt);
            }
            matrix_->repaint();
            refreshActions();
        });
    };
    actions_[6]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Exporter le motif"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("motif.json"), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting, [this, idx] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
            f.replaceWithText (juce::String (pattern::toJson ({ proc_.slotPattern (idx) }, "arpege")));
        });
    };
    actions_[7]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        proc_.clearSlot (idx);
        proc_.setSlotPattern (idx, proc_.slotPattern (idx));   // un motif vide, prêt à dessiner
        matrix_->repaint();
        refreshActions();
    };

    nova::configureConstrainer (constrainer_, kDesignW, kDesignH, 0.6f, 3.0f);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (kDesignW, kDesignH);
    refreshActions();
    startTimerHz (15);
}

ArpegeEditor::~ArpegeEditor() { stopTimer(); }

void ArpegeEditor::refreshActions()
{
    const int idx = proc_.slot->get() - 1;
    const auto all = proc_.builtinPatterns();
    const bool on = idx >= 0;
    juce::String grid = juce::String::fromUTF8 ("Grille ") + kGridNames[matrix_->grid];
    juce::String len = juce::String::fromUTF8 ("Durée "), bars = juce::String::fromUTF8 ("Mesures ");
    if (on)
    {
        const auto pt = proc_.slotPattern (idx);
        len << juce::String (pt.length.toBeats(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " temps";
        bars << pt.bars;
    }
    actions_[0]->setText (grid);
    actions_[1]->setText (len);
    actions_[2]->setText (bars);
    actions_[3]->setText (juce::String::fromUTF8 ("Fourni : ") + (all.empty() ? juce::String() : juce::String (all[(size_t) fournis_ % all.size()].name)));
    actions_[4]->setText ("Charger");
    actions_[5]->setText ("Importer");
    actions_[6]->setText ("Exporter");
    actions_[7]->setText (juce::String::fromUTF8 ("Effacer"));
    for (int i = 1; i < 8; ++i) if (i != 3) actions_[(size_t) i]->setDim (! on);
    actions_[0]->setDim (! on);
}

void ArpegeEditor::resized()
{
    skin_.setScale (nova::fitScale (getWidth(), getHeight(), kDesignW, kDesignH, 0.6f, 3.0f));
    const auto px = [this] (float v) { return skin_.ipx (v); };
    const float w[] = { 260, 110, 150, 120, 120, 120, 120, 100, 190, 100, 170, 130, 150 };
    const int rowOf[] = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2 };
    int x[4] = { px (16.0f), px (16.0f), px (16.0f), px (16.0f) };
    const int y0 = px (60.0f), h = px (30.0f), gap = px (40.0f);
    for (size_t i = 0; i < pills_.size(); ++i)
    {
        const int r = rowOf[i];
        pills_[i]->setBounds (x[r], y0 + r * gap, px (w[i]), h);
        x[r] += px (w[i] + 8.0f);
    }
    const float aw[] = { 190, 140, 110, 250, 100, 100, 100, 100 };
    const int aRow[] = { 2, 2, 2, 3, 3, 3, 3, 3 };
    for (size_t i = 0; i < actions_.size(); ++i)
    {
        const int r = aRow[i];
        actions_[i]->setBounds (x[r], y0 + r * gap, px (aw[i]), h);
        x[r] += px (aw[i] + 8.0f);
    }
    const int my = y0 + 4 * gap + px (4.0f);
    matrix_->setBounds (px (16.0f), my, getWidth() - px (32.0f), getHeight() - my - px (40.0f));
}

void ArpegeEditor::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (theme::Token::BgApp));
    nova::paintHeaderBar (g, skin_, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), skin_.px (46.0f)), juce::String::fromUTF8 ("Arpège"), juce::String::fromUTF8 ("arpégiateur"));
    g.setFont (skin_.font (13.0f));
    g.setColour (skin_.colour (theme::Token::TextDim));
    const auto text = juce::String::fromUTF8 ("Notes tenues : ") + juce::String (proc_.heldCount()) + "   ·   "
                      + (proc_.hostPlaying() ? juce::String::fromUTF8 ("calé sur la lecture de l'hôte") : juce::String::fromUTF8 ("lecture arrêtée : horloge interne"))
                      + juce::String::fromUTF8 ("   ·   glisser : régler  ·  clic : changer  ·  double-clic : saisir  ·  matrice : clic ou glisser, bande du bas : accent");
    g.drawText (text, juce::Rectangle<float> (skin_.px (16.0f), static_cast<float> (getHeight()) - skin_.px (34.0f), static_cast<float> (getWidth()) - skin_.px (32.0f), skin_.px (24.0f)).toNearestInt(),
                juce::Justification::centredLeft, true);
}

void ArpegeEditor::timerCallback()
{
    for (auto& p : pills_) p->refresh();
    const int idx = proc_.slot->get() - 1;
    if (idx != shownSlot_)
    {
        shownSlot_ = idx;
        if (idx >= 0 && ! proc_.slotUsed (idx)) proc_.setSlotPattern (idx, proc_.slotPattern (idx));   // emplacement libre : un motif vide, prêt à dessiner
        matrix_->repaint();
        refreshActions();
    }
    repaint (0, getHeight() - skin_.ipx (40.0f), getWidth(), skin_.ipx (40.0f));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArpegeProcessor(); }
