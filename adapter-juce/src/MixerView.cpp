#include "MixerView.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "neptune/core/ProbePool.h"
#include "TrackBundle.h"
#include "VelocityPanel.h"

using namespace neptune::core;
using neptune::core::theme::Token;

namespace
{
constexpr int kStripW = 58;
constexpr int kMinStripH = 288, kMaxStripH = 360;
constexpr int kPanelPad = 6, kPanelTitleH = 20, kPanelGap = 8;   // conteneurs « pistes » et « bus »
constexpr int kInsertRowH = 18, kVisibleInserts = 4;
constexpr int kMasterW = 140;   // deux colonnes : réglages à gauche, fader et vu-mètres à droite ; de la hauteur d'une bande
constexpr double kFaderMinDb = -100.0, kFaderMaxDb = 6.0;

juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

// Curseur lié à un paramètre : début / valeur / fin de geste passent par le processeur (annulation + automation de l'hôte).
struct BoundSlider : juce::Slider
{
    BoundSlider (NeptuneProcessor& p, juce::Slider::SliderStyle style) : juce::Slider (style, juce::Slider::NoTextBox), proc (p)
    {
        onDragStart = [this] { dragging = true; if (id != 0) proc.uiBeginEdit (id); };
        onDragEnd = [this] { dragging = false; if (id != 0) proc.uiEndEdit (id); };
        onValueChange = [this]
        {
            if (updating || id == 0) return;
            const auto* spec = proc.parameters().find (id);
            if (spec == nullptr) return;
            if (! dragging) proc.uiBeginEdit (id); // changement ponctuel (molette, clavier) : un geste complet
            proc.uiSetNormalised (id, spec->toNormalized (getValue()));
            if (! dragging) proc.uiEndEdit (id);
        };
    }
    void bind (ParamId pid, double resetValue)
    {
        id = pid;
        setDoubleClickReturnValue (true, resetValue);
    }
    // La modulation (hôte ou matrice) ne change pas la valeur réglée : le curseur garde sa place, le rail montre la valeur modulée.
    void syncModulation()
    {
        if (id == 0) return;
        const auto& set = proc.parameters();
        const auto index = set.indexOf (id);
        const float off = index != neptune::core::ParameterSet::npos ? set.modOffsetAt (index) : 0.0f;
        if (std::abs (off - lastOffset) <= 0.002f && (off == 0.0f) == (lastOffset == 0.0f)) return;
        lastOffset = off;
        if (off == 0.0f) getProperties().remove ("modValue");
        else
        {
            const auto& spec = set.spec (index);
            const double n = std::clamp (spec.toNormalized (getValue()) + static_cast<double> (off), 0.0, 1.0);
            getProperties().set ("modValue", spec.fromNormalized (n));
        }
        repaint();
    }
    void sync()
    {
        syncModulation();
        if (id == 0 || dragging || isMouseButtonDown()) return;
        updating = true;
        setValue (proc.parameters().get (id), juce::dontSendNotification);
        updating = false;
    }

    NeptuneProcessor& proc;
    ParamId id = 0;
    bool dragging = false, updating = false;
    float lastOffset = 0.0f;
};

// Bouton à bascule lié à un paramètre 0/1 (mute, solo, mono, parallèle). Dessiné sans les marges du style par défaut : une lettre
// tient dans un tiers de bande (le style par défaut tronquait « M », « S » et « P » en « ... »).
struct BoundToggle : juce::TextButton
{
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        // même figure que les pastilles NOVA : allumé = teinte de l'état en transparence, bord et texte à sa couleur
        const auto tint = findColour (buttonOnColourId);
        auto fill = on ? tint.withAlpha (0.22f) : findColour (buttonColourId);
        if (down) fill = fill.brighter (0.2f); else if (over) fill = fill.brighter (0.1f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (on ? tint.withAlpha (0.8f) : findColour (juce::ComboBox::outlineColourId).withAlpha (0.8f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        g.setColour (on ? tint : findColour (textColourOffId));
        g.setFont (juce::FontOptions (juce::jmin (12.5f, r.getHeight() * 0.6f), juce::Font::bold));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

    BoundToggle (NeptuneProcessor& p, const juce::String& label) : proc (p)
    {
        setButtonText (label);
        setClickingTogglesState (false);
        onClick = [this] { if (onToggle) onToggle (juce::ModifierKeys::currentModifiers.isShiftDown()); };
    }
    void sync()
    {
        const bool on = id != 0 && proc.parameters().get (id) >= 0.5;
        if (on != getToggleState()) setToggleState (on, juce::dontSendNotification);
    }
    void setValueFromUi (bool on)
    {
        if (id == 0) return;
        const auto* spec = proc.parameters().find (id);
        if (spec == nullptr) return;
        proc.uiBeginEdit (id);
        proc.uiSetNormalised (id, spec->toNormalized (on ? 1.0 : 0.0));
        proc.uiEndEdit (id);
    }
    NeptuneProcessor& proc;
    ParamId id = 0;
    std::function<void (bool shift)> onToggle;
};

float dbOf (float lin) { return lin <= 1e-5f ? -100.0f : 20.0f * std::log10 (lin); }
} // namespace

// ------------------------------------------------------------------------------------------------ fenêtre de routage

// Routage d'une bande : sortie principale, sortie directe, mode de pan, 8 envois (bus, point de captation, niveau, pan, mute).
// Courbe de vélocité d'une piste dans une bulle : les mêmes réglages que le widget Courbe de la Façade. Se remet à jour quand la
// courbe change ailleurs (annulation, Façade).
class TrackVelocityPanel : public VelocityPanel, private juce::Timer
{
public:
    TrackVelocityPanel (NeptuneProcessor& p, const theme::Palette& palette, const i18n::Translator& tr, int track)
        : VelocityPanel (palette, tr, 0, false, p.trackSettings (track).velocityCurve), proc_ (p), track_ (track)
    {
        setFadeControlsVisible (false);
        setEnabledToggleVisible (true);
        setSize (420, 450);
        onGestureBegin = [this] { proc_.uiBeginGesture(); };
        onGestureEnd = [this] { proc_.uiEndGesture(); };
        onUndo = [this] { proc_.uiUndo(); };
        onRedo = [this] { proc_.uiRedo(); };
        onChange = [this] (int, bool, const VelocityCurve& curve)
        {
            auto strip = proc_.trackSettings (track_);
            strip.velocityCurve = curve;
            proc_.uiEditTrack (track_, std::move (strip), "Velocity curve", "velocity_curve");
        };
        startTimerHz (8);
    }
    ~TrackVelocityPanel() override { stopTimer(); }

private:
    void timerCallback() override { setValues (0, false, proc_.trackSettings (track_).velocityCurve); }

    NeptuneProcessor& proc_;
    int track_;
};

// Canaux MIDI d'une piste : une grille de 16 cases, plusieurs canaux possibles à la fois. Sur « tous les canaux », un clic choisit ce canal
// seul (on part de lui pour en ajouter) ; décocher le dernier canal revient à « tous ».
class TrackChannelPanel : public juce::Component, private juce::Timer
{
public:
    TrackChannelPanel (NeptuneProcessor& p, const i18n::Translator& tr, int track, juce::Colour accent) : proc_ (p), track_ (track)
    {
        for (int c = 0; c < 16; ++c)
        {
            auto& b = buttons_[static_cast<std::size_t> (c)];
            b.setButtonText (juce::String (c + 1));
            b.setColour (juce::TextButton::buttonOnColourId, accent);
            b.onClick = [this, c] { toggle (c); };
            addAndMakeVisible (b);
        }
        all_.setButtonText (juce::String::fromUTF8 (std::string (tr.tr ("mix.midi_all")).c_str()));
        all_.setColour (juce::TextButton::buttonOnColourId, accent);
        all_.onClick = [this] { apply (0xFFFFu); };
        addAndMakeVisible (all_);
        setSize (4 * 56 + 5 * 6, 5 * 42 + 6 * 6);
        refresh();
        startTimerHz (8);
    }
    ~TrackChannelPanel() override { stopTimer(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        all_.setBounds (r.removeFromTop (42));
        r.removeFromTop (6);
        for (int row = 0; row < 4; ++row)
        {
            auto line = r.removeFromTop (42);
            r.removeFromTop (6);
            for (int col = 0; col < 4; ++col)
            {
                buttons_[static_cast<std::size_t> (row * 4 + col)].setBounds (line.removeFromLeft (56));
                line.removeFromLeft (6);
            }
        }
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        const unsigned mask = mixer::midiChannelsOf (proc_.trackSettings (track_));
        for (int c = 0; c < 16; ++c) buttons_[static_cast<std::size_t> (c)].setToggleState (((mask >> c) & 1u) != 0, juce::dontSendNotification);
        all_.setToggleState (mask == 0xFFFFu, juce::dontSendNotification);
    }

    void apply (unsigned mask)
    {
        auto strip = proc_.trackSettings (track_);
        mixer::setMidiChannels (strip, mask);
        proc_.uiEditTrack (track_, std::move (strip), "MIDI channels", "midi_channels");
        refresh();
    }

    void toggle (int c)
    {
        const unsigned mask = mixer::midiChannelsOf (proc_.trackSettings (track_));
        unsigned next = mask == 0xFFFFu ? (1u << c) : (mask ^ (1u << c));
        if (next == 0) next = 0xFFFFu;
        apply (next);
    }

    NeptuneProcessor& proc_;
    int track_;
    std::array<juce::TextButton, 16> buttons_;
    juce::TextButton all_;
};

// Texte court du bouton de canaux : A (tous), le numéro, « 1,3 » pour deux canaux, « 1+ » au-delà.
static juce::String channelLabel (unsigned mask)
{
    if (mask == 0xFFFFu) return "A";
    juce::StringArray list;
    for (int c = 0; c < 16; ++c)
        if (((mask >> c) & 1u) != 0) list.add (juce::String (c + 1));
    if (list.size() <= 2) return list.joinIntoString (",");
    return list[0] + "+";
}

class RoutingPanel : public juce::Component
{
public:
    // kind : 0 piste, 1 bus, 2 piste parallèle
    RoutingPanel (NeptuneProcessor& p, const theme::Palette& palette, const i18n::Translator& tr, int kind, int index)
        : proc_ (p), palette_ (palette), tr_ (tr), isBus_ (kind == 1), isPar_ (kind == 2), index_ (index)
    {
        const bool isBus = isBus_;
        if (isPar_) par_ = p.parallelSettings (index);
        s_ = isBus ? p.busSettings (index) : isPar_ ? par_.strip : p.trackSettings (index);
        auto text = [this] (const char* k) { return u8 (std::string (tr_.tr (k))); };
        auto label = [this] (juce::Label& l, const juce::String& t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, colour (Token::Text));
            addAndMakeVisible (l);
        };
        label (mainLabel_, text (isPar_ ? "mix.sum_parent_title" : "mix.main_out"));
        label (directLabel_, text ("mix.direct_out"));
        label (panLabel_, text ("mix.pan_mode"));
        mainBox_.addItem ("Master", 1);
        for (int b = 1; b <= mixer::numBuses; ++b)
        {
            mainBox_.addItem ("Bus " + juce::String (b), b + 1);
            if (isBus_ && b <= index_ + 1) mainBox_.setItemEnabled (b + 1, false); // un bus ne route que vers un bus supérieur
        }
        mainBox_.setSelectedId (s_.routing.mainDest + 1, juce::dontSendNotification);
        mainBox_.onChange = [this] { s_.routing.mainDest = mainBox_.getSelectedId() - 1; push(); };
        addAndMakeVisible (mainBox_);
        mainBox_.setVisible (! isPar_);
        mainOn_.setButtonText (text (isPar_ ? "mix.sum_parent" : "mix.main_on"));
        mainOn_.setToggleState (s_.routing.mainEnabled, juce::dontSendNotification);
        mainOn_.setColour (juce::ToggleButton::textColourId, colour (Token::Text));
        mainOn_.onClick = [this] { s_.routing.mainEnabled = mainOn_.getToggleState(); push(); };
        addAndMakeVisible (mainOn_);
        directBox_.addItem (text ("mix.none"), 1);
        for (int o = 1; o <= mixer::numOutputs; ++o) directBox_.addItem (text ("mix.output") + " " + juce::String (o), o + 1);
        directBox_.setSelectedId (s_.routing.directOut + 1, juce::dontSendNotification);
        directBox_.onChange = [this] { s_.routing.directOut = directBox_.getSelectedId() - 1; push(); };
        addAndMakeVisible (directBox_);
        panBox_.addItem (text ("mix.pan_balance"), 1);
        panBox_.addItem (text ("mix.pan_true"), 2);
        panBox_.setSelectedId (s_.panMode == mixer::PanMode::True ? 2 : 1, juce::dontSendNotification);
        panBox_.onChange = [this] { s_.panMode = panBox_.getSelectedId() == 2 ? mixer::PanMode::True : mixer::PanMode::Balance; push(); };
        addAndMakeVisible (panBox_);
        if (isPar_)
        {
            follow_.setButtonText (text ("mix.follow_parent"));
            follow_.setToggleState (par_.followParentVolume, juce::dontSendNotification);
            follow_.setColour (juce::ToggleButton::textColourId, colour (Token::Text));
            follow_.onClick = [this]
            {
                par_.strip = s_;
                par_.followParentVolume = follow_.getToggleState();
                proc_.uiEditParallel (index_, par_, "Parallel follow volume", "follow");
            };
            addAndMakeVisible (follow_);
        }

        for (int i = 0; i < mixer::numSends; ++i)
        {
            auto& r = rows_[static_cast<std::size_t> (i)];
            r.title.setText (text ("mix.send") + " " + juce::String (i + 1), juce::dontSendNotification);
            r.title.setColour (juce::Label::textColourId, colour (Token::TextDim));
            addAndMakeVisible (r.title);
            r.dest.addItem (text ("mix.none"), 1);
            for (int b = 1; b <= mixer::numBuses; ++b)
            {
                r.dest.addItem ("Bus " + juce::String (b), b + 1);
                if (isBus_ && b <= index_ + 1) r.dest.setItemEnabled (b + 1, false);
            }
            // pistes « Entrée audio » : destinations d'envoi supplémentaires (identifiant = destination + 1, après les bus)
            if (! isBus_)
                for (int t = 0; t < mixer::numTracks; ++t)
                {
                    const bool isAudioIn = proc_.trackActive (t) && proc_.trackType (t) == TrackType::AudioIn;
                    const bool current = s_.sends[static_cast<std::size_t> (i)].destination == mixer::firstTrackDestination + t;
                    if (! isAudioIn && ! current) continue;
                    r.dest.addItem (text ("mix.send_to_audio_in") + " " + juce::String (t + 1), mixer::firstTrackDestination + t + 1);
                    // pas vers soi-même, ni (depuis une Entrée audio) vers une piste d'indice inférieur ; l'origine d'une piste parallèle se juge au traitement
                    const bool fromAudioIn = ! isPar_ && proc_.trackType (index_) == TrackType::AudioIn;
                    if (! isPar_ && (t == index_ || (fromAudioIn && t < index_))) r.dest.setItemEnabled (mixer::firstTrackDestination + t + 1, false);
                    if (! isAudioIn) r.dest.setItemEnabled (mixer::firstTrackDestination + t + 1, false);
                }
            r.dest.setSelectedId (s_.sends[static_cast<std::size_t> (i)].destination + 1, juce::dontSendNotification);
            r.dest.onChange = [this, i] { s_.sends[static_cast<std::size_t> (i)].destination = rows_[static_cast<std::size_t> (i)].dest.getSelectedId() - 1; push(); };
            addAndMakeVisible (r.dest);
            r.tap.addItem (text ("mix.tap_pre_insert"), 1);
            r.tap.addItem (text ("mix.tap_pre_fader"), 2);
            r.tap.addItem (text ("mix.tap_post_fader"), 3);
            r.tap.addItem (text ("mix.tap_post_pan"), 4);
            r.tap.setSelectedId (static_cast<int> (s_.sends[static_cast<std::size_t> (i)].tap) + 1, juce::dontSendNotification);
            r.tap.onChange = [this, i] { s_.sends[static_cast<std::size_t> (i)].tap = static_cast<mixer::TapPoint> (rows_[static_cast<std::size_t> (i)].tap.getSelectedId() - 1); push(); };
            addAndMakeVisible (r.tap);
            const ParamId lid = isBus_ ? mixer::busParam (index_, mixer::kBusSendLevel + static_cast<ParamId> (i))
                              : isPar_ ? mixer::parallelParam (index_, mixer::kParSendLevel + static_cast<ParamId> (i))
                                       : probe::trackParam (index_, probe::kSendLevel + static_cast<ParamId> (i));
            r.level = std::make_unique<BoundSlider> (proc_, juce::Slider::LinearHorizontal);
            r.level->setRange (kFaderMinDb, 6.0, 0.1);
            r.level->setSkewFactorFromMidPoint (-24.0);
            r.level->bind (lid, -100.0);
            r.level->sync();
            addAndMakeVisible (*r.level);
            r.pan.setSliderStyle (juce::Slider::LinearHorizontal);
            r.pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            r.pan.setRange (-1.0, 1.0, 0.01);
            r.pan.setDoubleClickReturnValue (true, 0.0);
            r.pan.setValue (s_.sends[static_cast<std::size_t> (i)].pan, juce::dontSendNotification);
            r.pan.onValueChange = [this, i] { s_.sends[static_cast<std::size_t> (i)].pan = rows_[static_cast<std::size_t> (i)].pan.getValue(); push(); };
            addAndMakeVisible (r.pan);
            r.mute.setButtonText ("M");
            r.mute.setClickingTogglesState (true);
            r.mute.setToggleState (s_.sends[static_cast<std::size_t> (i)].mute, juce::dontSendNotification);
            r.mute.onClick = [this, i] { s_.sends[static_cast<std::size_t> (i)].mute = rows_[static_cast<std::size_t> (i)].mute.getToggleState(); push(); };
            addAndMakeVisible (r.mute);
        }
        setSize (700, isPar_ ? 470 : 436);
    }

    void paint (juce::Graphics& g) override { g.fillAll (colour (Token::BgCard)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto row = [&r] (juce::Label& l, juce::Component& c, int lw = 130)
        {
            auto line = r.removeFromTop (32);
            l.setBounds (line.removeFromLeft (lw));
            c.setBounds (line.removeFromLeft (220).reduced (2));
            return line;
        };
        auto extra = row (mainLabel_, mainBox_);
        mainOn_.setBounds (extra.reduced (2));
        row (directLabel_, directBox_);
        row (panLabel_, panBox_);
        if (isPar_) follow_.setBounds (r.removeFromTop (30));
        r.removeFromTop (8);
        for (auto& row2 : rows_)
        {
            auto line = r.removeFromTop (32);
            row2.title.setBounds (line.removeFromLeft (64));
            row2.dest.setBounds (line.removeFromLeft (120).reduced (1, 2));
            row2.tap.setBounds (line.removeFromLeft (150).reduced (1, 2));
            row2.level->setBounds (line.removeFromLeft (160).reduced (1));
            row2.pan.setBounds (line.removeFromLeft (90).reduced (1));
            row2.mute.setBounds (line.removeFromLeft (34).reduced (1, 2));
        }
    }

    void refreshLevels() { for (auto& r : rows_) r.level->sync(); }

private:
    juce::Colour colour (Token t) const
    {
        const auto& c = palette_.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    }
    void push()
    {
        if (isPar_)
        {
            par_ = proc_.parallelSettings (index_); // garde la captation et le suivi tels qu'ils sont ailleurs
            par_.strip = s_;
            proc_.uiEditParallel (index_, par_, "Parallel routing", "routing");
        }
        else if (isBus_) proc_.uiEditBus (index_, s_, "Bus routing", "routing");
        else proc_.uiEditTrack (index_, s_, "Track routing", "routing");
    }

    struct Row
    {
        juce::Label title;
        juce::ComboBox dest, tap;
        std::unique_ptr<BoundSlider> level;
        juce::Slider pan;
        juce::TextButton mute;
    };

    NeptuneProcessor& proc_;
    theme::Palette palette_;
    const i18n::Translator& tr_;
    bool isBus_;
    bool isPar_ = false;
    int index_;
    mixer::StripSettings s_;
    mixer::ParallelSettings par_;
    juce::ToggleButton follow_;
    juce::Label mainLabel_, directLabel_, panLabel_;
    juce::ComboBox mainBox_, directBox_, panBox_;
    juce::ToggleButton mainOn_;
    std::array<Row, mixer::numSends> rows_;
};

// ------------------------------------------------------------------------------------------------ inserts

// Les 8 emplacements d'inserts d'une bande : 4 lignes visibles, les 4 suivantes par la molette ou la flèche. Vide : « + ».
class MixerView::InsertList : public juce::Component
{
public:
    InsertList (MixerView& owner, NeptuneProcessor::StripKind kind, int index) : owner_ (owner), kind_ (kind), index_ (index) {}

    static int heightNeeded() { return kInsertRowH * kVisibleInserts + 2; }

    void refresh()
    {
        bool changed = false;
        for (int s = 0; s < mixer::numInserts; ++s)
        {
            const auto type = owner_.processor_.insertType (kind_, index_, s);
            const bool by = ! type.empty() && owner_.processor_.insertBypassed (kind_, index_, s);
            if (type != types_[static_cast<std::size_t> (s)] || by != bypassed_[static_cast<std::size_t> (s)]) changed = true;
            types_[static_cast<std::size_t> (s)] = type;
            bypassed_[static_cast<std::size_t> (s)] = by;
        }
        if (changed) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // une puce par emplacement : point d'état + nom ; vide : pointillé discret et « + »
        for (int row = 0; row < kVisibleInserts; ++row)
        {
            const int slot = page_ * kVisibleInserts + row;
            const auto r = rowRect (row).toFloat().reduced (0.0f, 1.0f);
            const bool used = ! types_[static_cast<std::size_t> (slot)].empty();
            const bool by = bypassed_[static_cast<std::size_t> (slot)];
            if (used)
            {
                g.setColour (owner_.colour (Token::BgApp));
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (owner_.colour (Token::Border));
                g.drawRoundedRectangle (r, 4.0f, 1.0f);
                g.setColour (owner_.colour (by ? Token::TextDim2 : Token::Accent));
                g.fillEllipse (r.getX() + 5.0f, r.getCentreY() - 2.5f, 5.0f, 5.0f);
                g.setColour (owner_.colour (by ? Token::TextDim2 : Token::Text));
                g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                g.drawText (name (types_[static_cast<std::size_t> (slot)]).toUpperCase(), r.withTrimmedLeft (13.0f).toNearestInt(), juce::Justification::centredLeft, true);
            }
            else
            {
                g.setColour (owner_.colour (Token::Border).withAlpha (0.7f));
                g.drawRoundedRectangle (r, 4.0f, 1.0f);
                g.setColour (owner_.colour (Token::TextDim));
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("+", r.toNearestInt(), juce::Justification::centred);
            }
        }
        // pagination : 1-4 / 5-8
        g.setColour (owner_.colour (Token::TextDim));
        const auto a = arrowRect();
        juce::Path tri;
        if (page_ == 0) tri.addTriangle ((float) a.getX() + 1, (float) a.getY() + 4, (float) a.getRight() - 1, (float) a.getY() + 4, a.getCentreX(), (float) a.getBottom() - 4);
        else tri.addTriangle ((float) a.getX() + 1, (float) a.getBottom() - 4, (float) a.getRight() - 1, (float) a.getBottom() - 4, a.getCentreX(), (float) a.getY() + 4);
        g.fillPath (tri);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (arrowRect().contains (e.getPosition())) { page_ = 1 - page_; repaint(); return; }
        const int row = e.y / kInsertRowH;
        if (row < 0 || row >= kVisibleInserts) return;
        const int slot = page_ * kVisibleInserts + row;
        const bool used = ! types_[static_cast<std::size_t> (slot)].empty();
        if (e.mods.isPopupMenu() && used) { showMenu (slot); return; }
        if (used) { if (owner_.onOpenInsert) owner_.onOpenInsert (kind_, index_, slot); }
        else showMenu (slot);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int next = w.deltaY < 0 ? 1 : 0;
        if (w.deltaY != 0.0f && next != page_) { page_ = next; repaint(); }
    }

private:
    juce::Rectangle<int> rowRect (int row) const { return { 0, row * kInsertRowH, getWidth() - 12, kInsertRowH }; }
    juce::Rectangle<int> arrowRect() const { return { getWidth() - 12, 0, 12, kInsertRowH * kVisibleInserts }; }

    juce::String name (const std::string& type) const
    {
        for (const auto& m : availableModules())
            if (m.typeId == type) return u8 (std::string (owner_.tr_.tr (m.nameKey)));
        return u8 (type);
    }

    void showMenu (int slot)
    {
        const bool used = ! types_[static_cast<std::size_t> (slot)].empty();
        juce::PopupMenu menu;
        const auto& modules = availableModules();
        if (! used)
        {
            for (std::size_t i = 0; i < modules.size(); ++i)
                menu.addItem (static_cast<int> (i) + 1, u8 (std::string (owner_.tr_.tr (modules[i].nameKey))));
        }
        else
        {
            menu.addItem (1, u8 (std::string (owner_.tr_.tr ("insert.open"))));
            menu.addItem (2, u8 (std::string (owner_.tr_.tr ("insert.bypass"))), true, bypassed_[static_cast<std::size_t> (slot)]);
            menu.addItem (3, u8 (std::string (owner_.tr_.tr ("insert.remove"))));
        }
        menu.setLookAndFeel (&getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (220),
                            [safe = juce::Component::SafePointer<InsertList> (this), slot, used] (int choice)
                            {
                                if (safe == nullptr || choice == 0) return;
                                auto& proc = safe->owner_.processor_;
                                const auto kind = safe->kind_;
                                const int index = safe->index_;
                                if (! used)
                                {
                                    const auto& mods = availableModules();
                                    if (choice < 1 || choice > static_cast<int> (mods.size())) return;
                                    proc.uiSetInsert (kind, index, slot, std::string (mods[static_cast<std::size_t> (choice - 1)].typeId));
                                    safe->refresh();
                                    if (safe->owner_.onOpenInsert) safe->owner_.onOpenInsert (kind, index, slot);
                                }
                                else if (choice == 1) { if (safe->owner_.onOpenInsert) safe->owner_.onOpenInsert (kind, index, slot); }
                                else if (choice == 2)
                                {
                                    const auto id = proc.insertBypassParam (kind, index, slot);
                                    const auto* spec = proc.parameters().find (id);
                                    if (spec == nullptr) return;
                                    proc.uiBeginEdit (id);
                                    proc.uiSetNormalised (id, spec->toNormalized (proc.parameters().get (id) < 0.5 ? 1.0 : 0.0));
                                    proc.uiEndEdit (id);
                                }
                                else if (choice == 3)
                                {
                                    proc.uiSetInsert (kind, index, slot, std::string());
                                    safe->refresh();
                                }
                            });
    }

    MixerView& owner_;
    NeptuneProcessor::StripKind kind_;
    int index_;
    int page_ = 0;
    std::array<std::string, mixer::numInserts> types_;
    std::array<bool, mixer::numInserts> bypassed_ {};
};

// ------------------------------------------------------------------------------------------------ une bande

class MixerView::Strip : public juce::Component
{
public:
    // kind : 0 piste, 1 bus, 2 piste parallèle
    Strip (MixerView& owner, int kind, int index)
        : owner_ (owner), isBus_ (kind == 1), isPar_ (kind == 2), index_ (index),
          pan_ (owner.processor_, juce::Slider::LinearHorizontal), fader_ (owner.processor_, juce::Slider::LinearVertical),
          mute_ (owner.processor_, "M"), solo_ (owner.processor_, "S"), parallel_ (owner.processor_, "P"),
          inserts_ (owner, kind == 1 ? NeptuneProcessor::StripKind::Bus : kind == 2 ? NeptuneProcessor::StripKind::Parallel : NeptuneProcessor::StripKind::Track, index)
    {
        const bool isBus = isBus_;
        auto& proc = owner.processor_;
        addAndMakeVisible (inserts_);
        name_.setJustificationType (juce::Justification::centred);
        name_.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        name_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        name_.setEditable (false, true, false);
        name_.onTextChange = [this]
        {
            auto s = settings();
            s.name = name_.getText().toStdString();
            commit (s);
        };
        name_.addMouseListener (this, false);
        addAndMakeVisible (name_);

        if (isPar_)
        {
            // en-tête : la parenté, le repliement des petites-filles et le retrait ; en dessous, le point de captation
            typeButton_.setEnabled (false);
            addAndMakeVisible (typeButton_);
            removeButton_.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
            removeButton_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.remove_parallel_tip"))));
            removeButton_.onClick = [this] { owner_.requestRemoveParallel (index_); };
            addAndMakeVisible (removeButton_);
            foldButton_.onClick = [this] { owner_.toggleFold (index_); };
            addAndMakeVisible (foldButton_);
            const char* taps[4] = { "mix.tap_pre_insert", "mix.tap_pre_fader", "mix.tap_post_fader", "mix.tap_post_pan" };
            for (int i = 0; i < 4; ++i) tapBox_.addItem (u8 (std::string (owner.tr_.tr (taps[i]))), i + 1);
            tapBox_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.capture_tip"))));
            tapBox_.onChange = [this]
            {
                auto ps = owner_.processor_.parallelSettings (index_);
                ps.tap = static_cast<mixer::TapPoint> (tapBox_.getSelectedId() - 1);
                owner_.processor_.uiEditParallel (index_, ps, "Parallel capture", "capture");
            };
            addAndMakeVisible (tapBox_);
        }
        if (! isBus && ! isPar_)
        {
            typeButton_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.type_tip"))));
            typeButton_.onClick = [this] { showTypeMenu(); };
            addAndMakeVisible (typeButton_);
            removeButton_.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
            removeButton_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.remove_tip"))));
            removeButton_.onClick = [this] { owner_.requestRemove (index_); };
            addAndMakeVisible (removeButton_);
            // canaux MIDI : « A » (tous), le numéro, ou plusieurs ; un clic ouvre la grille des 16 canaux
            midi_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
            midi_.onClick = [this]
            {
                auto& box = juce::CallOutBox::launchAsynchronously (std::make_unique<TrackChannelPanel> (owner_.processor_, owner_.tr_, index_, owner_.colour (Token::Accent)),
                                                                    midi_.getScreenBounds(), nullptr);
                box.setLookAndFeel (&midi_.getLookAndFeel());
            };
            addAndMakeVisible (midi_);
            // « V » : la courbe de vélocité de la piste, allumée quand elle agit
            velocityButton_.setButtonText (u8 (std::string (owner.tr_.tr ("mix.velocity_short"))));
            velocityButton_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.velocity_tip"))));
            velocityButton_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
            velocityButton_.onClick = [this]
            {
                auto& box = juce::CallOutBox::launchAsynchronously (std::make_unique<TrackVelocityPanel> (owner_.processor_, owner_.palette_, owner_.tr_, index_),
                                                                    velocityButton_.getScreenBounds(), nullptr);
                box.setLookAndFeel (&velocityButton_.getLookAndFeel());
            };
            addAndMakeVisible (velocityButton_);
        }
        routing_.setButtonText (juce::String::fromUTF8 (std::string (owner.tr_.tr ("mix.routing_short")).c_str()));
        routing_.onClick = [this]
        {
            auto panel = std::make_unique<RoutingPanel> (owner_.processor_, owner_.palette_, owner_.tr_, isBus_ ? 1 : isPar_ ? 2 : 0, index_);
            panelRef_ = panel.get();
            // la bulle prend le style NOVA de la barre (cadre, listes, curseurs), pas celui de JUCE
            auto& box = juce::CallOutBox::launchAsynchronously (std::move (panel), routing_.getScreenBounds(), nullptr);
            box.setLookAndFeel (&routing_.getLookAndFeel());
        };
        addAndMakeVisible (routing_);

        pan_.setRange (-100.0, 100.0, 1.0);
        fader_.setRange (kFaderMinDb, kFaderMaxDb, 0.1);
        fader_.setSkewFactorFromMidPoint (-24.0);
        const ParamId pv = isBus ? mixer::busParam (index, mixer::kBusVolume) : isPar_ ? mixer::parallelParam (index, mixer::kParVolume) : probe::trackParam (index, probe::kVolume);
        const ParamId pp = isBus ? mixer::busParam (index, mixer::kBusPan) : isPar_ ? mixer::parallelParam (index, mixer::kParPan) : probe::trackParam (index, probe::kPan);
        const ParamId pm = isBus ? mixer::busParam (index, mixer::kBusMute) : isPar_ ? mixer::parallelParam (index, mixer::kParMute) : probe::trackParam (index, probe::kMute);
        const ParamId ps = isBus ? mixer::busParam (index, mixer::kBusSolo) : isPar_ ? mixer::parallelParam (index, mixer::kParSolo) : probe::trackParam (index, probe::kSolo);
        pan_.bind (pp, 0.0);
        fader_.bind (pv, 0.0);
        pan_.getProperties().set ("bipolar", true);
        pan_.getProperties().set ("soft", true);   // le point du curseur est gris plutôt que blanc plein
        fader_.getProperties().set ("fader", true);
        addAndMakeVisible (pan_);
        addAndMakeVisible (fader_);
        mute_.id = pm;
        solo_.id = ps;
        mute_.onToggle = [this, &proc] (bool)
        {
            mute_.setValueFromUi (proc.parameters().get (mute_.id) < 0.5);
        };
        solo_.onToggle = [this, &proc] (bool shift)
        {
            const bool turnOn = proc.parameters().get (solo_.id) < 0.5;
            if (shift && turnOn) owner_.exclusiveSolo (isBus_ ? 1 : isPar_ ? 2 : 0, index_); // solo exclusif : annule les autres solos
            solo_.setValueFromUi (turnOn);
        };
        mute_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::KnobHot));
        solo_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
        addAndMakeVisible (mute_);
        addAndMakeVisible (solo_);
        if (! isBus_)
        {
            // « P » : crée une piste parallèle à droite de cette bande, dans sa chaîne
            parallel_.setTooltip (u8 (std::string (owner.tr_.tr ("mix.add_parallel_tip"))));
            parallel_.onToggle = [this] (bool) { owner_.requestAddParallel (isPar_ ? mixer::numTracks + index_ : index_); };
            addAndMakeVisible (parallel_);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isBus_ && ! isPar_ && owner_.onSelectTrack && e.eventComponent != &fader_ && e.eventComponent != &pan_) owner_.onSelectTrack (index_);
    }

    void refresh (bool selected)
    {
        selected_ = selected;
        if (isPar_)
        {
            const auto ps = owner_.processor_.parallelSettings (index_);
            const juce::String parent = ps.parent < mixer::numTracks ? juce::String (ps.parent + 1) : "P" + juce::String (ps.parent - mixer::numTracks + 1);
            typeButton_.setButtonText ("P" + juce::String (index_ + 1) + " <" + parent);
            tapBox_.setSelectedId (static_cast<int> (ps.tap) + 1, juce::dontSendNotification);
            int kids = 0;
            for (const int c : owner_.processor_.parallelChildren (mixer::numTracks + index_)) kids += 1 + static_cast<int> (owner_.processor_.parallelChildren (mixer::numTracks + c).size());
            hasFold_ = owner_.processor_.parallelDepth (index_) >= 1 && ! owner_.processor_.parallelChildren (mixer::numTracks + index_).empty();
            foldButton_.setButtonText ((owner_.isExpanded (index_) ? juce::String::fromUTF8 ("\xe2\x96\xbe") : juce::String::fromUTF8 ("\xe2\x96\xb8")) + juce::String (kids));
            foldButton_.setVisible (hasFold_);
            depth_ = owner_.processor_.parallelDepth (index_);
        }
        if (! isBus_ && ! isPar_)
        {
            const auto type = owner_.processor_.trackType (index_);
            const char* key = type == TrackType::AudioIn ? "mix.type_short_audio" : type == TrackType::Organ ? "mix.type_short_organ" : type == TrackType::Fm ? "mix.type_short_fm" : type == TrackType::Synth ? "mix.type_short_synth" : "mix.type_short_sampler";
            typeButton_.setButtonText (u8 (std::string (owner_.tr_.tr (key))));
        }
        const auto s = settings();
        const juce::String fallback = (isBus_ ? "Bus " : isPar_ ? "P" : "") + juce::String (index_ + 1);
        name_.setText (s.name.empty() ? fallback : u8 (s.name), juce::dontSendNotification);
        if (! isBus_ && ! isPar_)
        {
            const unsigned mask = mixer::midiChannelsOf (s);
            midi_.setButtonText (channelLabel (mask));
            midi_.setToggleState (mask != 0xFFFFu, juce::dontSendNotification);   // allumé quand la piste n'écoute pas tous les canaux
            juce::StringArray names;
            for (int c = 0; c < 16; ++c)
                if (((mask >> c) & 1u) != 0) names.add (juce::String (c + 1));
            midi_.setTooltip (u8 (std::string (owner_.tr_.tr ("mix.midi_tip"))) + "  " + (mask == 0xFFFFu ? u8 (std::string (owner_.tr_.tr ("mix.midi_all"))) : names.joinIntoString (", ")));
            velocityButton_.setToggleState (s.velocityCurve.active(), juce::dontSendNotification);
        }
        colourIndex_ = s.color;
        repaint();
    }

    void syncValues()
    {
        pan_.sync();
        fader_.sync();
        mute_.sync();
        solo_.sync();
        inserts_.refresh();
        if (panelRef_ != nullptr) panelRef_->refreshLevels();
    }

    void updateMeter (const Engine::Meter& m)
    {
        auto fall = [] (float current, float incoming) { return incoming > current ? incoming : current * 0.88f; };
        peakL_ = fall (peakL_, m.peakL);
        peakR_ = fall (peakR_, m.peakR);
        repaint (meterArea());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        auto head = r.removeFromTop (24);
        head.removeFromLeft (14);   // pastille de couleur (dessinée)
        if (! isBus_)
        {
            removeButton_.setBounds (head.removeFromRight (20).reduced (0, 1));
            if (isPar_ && hasFold_) foldButton_.setBounds (head.removeFromRight (26).reduced (0, 1));
            typeButton_.setBounds (head.reduced (0, 1));
        }
        if (isPar_) tapBox_.setBounds (r.removeFromTop (22).reduced (0, 1));
        else if (! isBus_)
        {
            auto row = r.removeFromTop (22).reduced (0, 1);
            velocityButton_.setBounds (row.removeFromRight (28));
            row.removeFromRight (2);
            midi_.setBounds (row);
        }
        routing_.setBounds (r.removeFromTop (22).reduced (0, 1));
        r.removeFromTop (2);
        inserts_.setBounds (r.removeFromTop (InsertList::heightNeeded() + 2).reduced (0, 1));
        panText_ = r.removeFromTop (13);
        pan_.setBounds (r.removeFromTop (18));
        name_.setBounds (r.removeFromBottom (20));
        auto bottom = r.removeFromBottom (24);
        if (isBus_)
        {
            mute_.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2).reduced (1));
            solo_.setBounds (bottom.reduced (1));
        }
        else
        {
            const int w = bottom.getWidth() / 3;
            mute_.setBounds (bottom.removeFromLeft (w).reduced (1));
            solo_.setBounds (bottom.removeFromLeft (w).reduced (1));
            parallel_.setBounds (bottom.reduced (1));
        }
        r.removeFromBottom (16); // valeur en dB
        faderArea_ = r;
        fader_.setBounds (r.removeFromLeft (r.getWidth() - 14));
    }

    void paint (juce::Graphics& g) override
    {
        const auto card = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (owner_.colour (isPar_ ? Token::BgApp : Token::BgCard));   // la sélection ne se marque que par le filet orange, sans voile
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (owner_.colour (selected_ ? Token::Accent : Token::Border));
        g.drawRoundedRectangle (card, 8.0f, selected_ ? 1.5f : 1.0f);
        if (isPar_) // une piste parallèle : bord d'accent à gauche, plus marqué en profondeur
        {
            g.setColour (owner_.colour (Token::Accent).withAlpha (depth_ >= 2 ? 0.55f : 1.0f));
            g.fillRoundedRectangle (1.0f, 8.0f, 3.0f, static_cast<float> (getHeight()) - 16.0f, 1.5f);
        }
        // pastille de couleur de la bande
        static const juce::uint32 palette[16] = { 0xff5c6bc0, 0xff26a69a, 0xffffa726, 0xffef5350, 0xff8d6e63, 0xff66bb6a, 0xffab47bc, 0xff42a5f5,
                                                 0xffffee58, 0xffec407a, 0xff78909c, 0xff26c6da, 0xffd4e157, 0xffff7043, 0xff7e57c2, 0xffbdbdbd };
        g.setColour (juce::Colour (palette[colourIndex_ & 15]));
        g.fillEllipse (8.0f, 11.0f, 8.0f, 8.0f);
        if (isBus_) // le haut d'une bande de bus : l'étiquette que les pistes remplacent par leur type de générateur
        {
            const juce::Rectangle<float> tag (static_cast<float> (getWidth()) - 34.0f, 7.0f, 28.0f, 16.0f);
            g.setColour (owner_.colour (Token::Border));
            g.drawRoundedRectangle (tag, 5.0f, 1.0f);
            g.setColour (owner_.colour (Token::TextDim));
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText ("BUS", tag.toNearestInt(), juce::Justification::centred);
        }
        // balance : C, L24, R12
        {
            const int v = juce::roundToInt (pan_.getValue());
            g.setColour (owner_.colour (Token::TextDim));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (v == 0 ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String (std::abs (v)), panText_, juce::Justification::centred);
        }

        // vu-mètre stéréo
        const auto m = meterArea();
        auto bar = [&] (float lin, int x)
        {
            const float db = dbOf (lin);
            const float h = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f) * static_cast<float> (m.getHeight());
            g.setColour (owner_.colour (Token::Grid));
            g.fillRoundedRectangle (static_cast<float> (x), static_cast<float> (m.getY()), 5.0f, static_cast<float> (m.getHeight()), 2.5f);
            const juce::Rectangle<float> level (static_cast<float> (x), static_cast<float> (m.getBottom()) - h, 5.0f, h);
            const auto top = db > 0.0f ? owner_.colour (Token::KnobHot) : owner_.colour (Token::Accent).brighter (0.3f);
            g.setGradientFill (juce::ColourGradient (top, level.getX(), level.getY(), owner_.colour (Token::Accent), level.getX(), level.getBottom(), false));
            g.fillRoundedRectangle (level, 2.5f);
        };
        bar (peakL_, m.getX());
        bar (peakR_, m.getX() + 6);
        g.setColour (owner_.colour (Token::Text));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (fader_.getValue() <= kFaderMinDb ? juce::String::fromUTF8 ("-inf") : juce::String (fader_.getValue(), 1) + " dB",
                    2, getHeight() - 62, getWidth() - 4, 14, juce::Justification::centred);
    }

private:
    void showTypeMenu()
    {
        const auto current = owner_.processor_.trackType (index_);
        juce::PopupMenu menu;
        menu.addItem (1, u8 (std::string (owner_.tr_.tr ("mix.type_sampler"))), true, current == TrackType::Sampler);
        menu.addItem (2, u8 (std::string (owner_.tr_.tr ("mix.type_synth"))), true, current == TrackType::Synth);
        menu.addSeparator();
        menu.addItem (3, u8 (std::string (owner_.tr_.tr ("mix.type_fm"))), true, current == TrackType::Fm);
        menu.addItem (5, u8 (std::string (owner_.tr_.tr ("mix.type_organ"))), true, current == TrackType::Organ);
        menu.addItem (4, u8 (std::string (owner_.tr_.tr ("mix.type_audio"))), true, current == TrackType::AudioIn);
        menu.addSeparator();
        menu.addItem (10, u8 (std::string (owner_.tr_.tr ("track.export_menu"))));
        menu.addItem (11, u8 (std::string (owner_.tr_.tr ("track.import_menu"))));
        menu.setLookAndFeel (&typeButton_.getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&typeButton_).withMinimumWidth (220),
                            [safe = juce::Component::SafePointer<Strip> (this)] (int choice)
                            {
                                if (safe == nullptr || choice == 0) return;
                                if (choice == 10) { safe->owner_.requestExportTrack (safe->index_); return; }
                                if (choice == 11) { safe->owner_.requestImportTrack(); return; }
                                safe->owner_.requestTypeChange (safe->index_, choice == 4 ? TrackType::AudioIn : choice == 5 ? TrackType::Organ : choice == 3 ? TrackType::Fm : choice == 2 ? TrackType::Synth : TrackType::Sampler);
                            });
    }

    juce::Rectangle<int> meterArea() const { return { getWidth() - 16, faderArea_.getY(), 12, faderArea_.getHeight() }; }
    mixer::StripSettings settings() const
    {
        if (isPar_) return owner_.processor_.parallelSettings (index_).strip;
        return isBus_ ? owner_.processor_.busSettings (index_) : owner_.processor_.trackSettings (index_);
    }
    void commit (mixer::StripSettings s)
    {
        if (isPar_)
        {
            auto ps = owner_.processor_.parallelSettings (index_);
            ps.strip = std::move (s);
            owner_.processor_.uiEditParallel (index_, std::move (ps), "Parallel settings", "strip");
        }
        else if (isBus_) owner_.processor_.uiEditBus (index_, std::move (s), "Bus settings", "strip");
        else owner_.processor_.uiEditTrack (index_, std::move (s), "Track settings", "strip");
    }
    static juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    MixerView& owner_;
    bool isBus_;
    bool isPar_ = false;
    int index_;
    bool selected_ = false;
    bool hasFold_ = false;
    int depth_ = 0;
    int colourIndex_ = 0;
    float peakL_ = 0.0f, peakR_ = 0.0f;
    juce::Label name_;
    juce::TextButton midi_;
    juce::TextButton typeButton_, removeButton_, foldButton_, velocityButton_;
    juce::ComboBox tapBox_;
    juce::TextButton routing_;
    BoundSlider pan_, fader_;
    BoundToggle mute_, solo_, parallel_;
    juce::Rectangle<int> faderArea_, panText_;
    juce::Component::SafePointer<RoutingPanel> panelRef_;
    InsertList inserts_;

    friend class MixerView;
};

// ------------------------------------------------------------------------------------------------ master

class MixerView::MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (MixerView& owner)
        : owner_ (owner), pan_ (owner.processor_, juce::Slider::LinearHorizontal), trim_ (owner.processor_, juce::Slider::LinearHorizontal),
          fader_ (owner.processor_, juce::Slider::LinearVertical), mute_ (owner.processor_, "M"), mono_ (owner.processor_, "Mono"),
          inserts_ (owner, NeptuneProcessor::StripKind::Master, 0)
    {
        addAndMakeVisible (inserts_);
        title_.setText ("MASTER", juce::dontSendNotification);
        title_.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title_.setJustificationType (juce::Justification::centred);
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        pan_.setRange (-100.0, 100.0, 1.0);
        pan_.bind (probe::masterPan, 0.0);
        pan_.getProperties().set ("bipolar", true);
        pan_.getProperties().set ("soft", true);   // le point du curseur est gris plutôt que blanc plein
        trim_.getProperties().set ("bipolar", true);
        trim_.getProperties().set ("soft", true);
        fader_.getProperties().set ("fader", true);
        trim_.setRange (-12.0, 12.0, 0.1);
        trim_.bind (mixer::masterTrim, 0.0);
        fader_.setRange (kFaderMinDb, kFaderMaxDb, 0.1);
        fader_.setSkewFactorFromMidPoint (-24.0);
        fader_.bind (probe::masterVolume, 0.0);
        mute_.id = probe::masterMute;
        mono_.id = mixer::masterMono;
        mute_.onToggle = [this] (bool) { mute_.setValueFromUi (owner_.processor_.parameters().get (mute_.id) < 0.5); };
        mono_.onToggle = [this] (bool) { mono_.setValueFromUi (owner_.processor_.parameters().get (mono_.id) < 0.5); };
        mute_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::KnobHot));
        mono_.setColour (juce::TextButton::buttonOnColourId, owner.colour (Token::Accent));
        for (auto* c : std::initializer_list<juce::Component*> { &pan_, &trim_, &fader_, &mute_, &mono_ }) addAndMakeVisible (c);
        trimLabel_.setText ("Trim", juce::dontSendNotification);
        panLabel_.setText ("Pan", juce::dontSendNotification);
        for (auto* l : { &trimLabel_, &panLabel_ })
        {
            l->setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
            l->setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (*l);
        }
        for (int o = 1; o <= mixer::numOutputs; ++o) output_.addItem ("Out " + juce::String (o), o);
        output_.onChange = [this]
        {
            auto m = owner_.processor_.masterSettings();
            m.output = output_.getSelectedId();
            owner_.processor_.uiEditMaster (m, "Master output", "master");
        };
        addAndMakeVisible (output_);
        panMode_.addItem ("Balance", 1);
        panMode_.addItem ("True pan", 2);
        panMode_.onChange = [this]
        {
            auto m = owner_.processor_.masterSettings();
            m.panMode = panMode_.getSelectedId() == 2 ? mixer::PanMode::True : mixer::PanMode::Balance;
            owner_.processor_.uiEditMaster (m, "Master pan mode", "master");
        };
        addAndMakeVisible (panMode_);
        lufsReset_.setButtonText ("LUFS 0");
        lufsReset_.setTooltip ("Reset integrated loudness");
        lufsReset_.onClick = [this] { owner_.processor_.resetLoudness(); };
        addAndMakeVisible (lufsReset_);
    }

    void refresh()
    {
        const auto m = owner_.processor_.masterSettings();
        output_.setSelectedId (m.output, juce::dontSendNotification);
        panMode_.setSelectedId (m.panMode == mixer::PanMode::True ? 2 : 1, juce::dontSendNotification);
    }

    void syncValues()
    {
        pan_.sync(); trim_.sync(); fader_.sync(); mute_.sync(); mono_.sync();
        inserts_.refresh();
    }

    void updateLoudness (const neptune::core::LoudnessMeter::Reading& r)
    {
        if (r.momentary != lufs_.momentary || r.shortTerm != lufs_.shortTerm || r.integrated != lufs_.integrated || r.truePeakDb != lufs_.truePeakDb)
        {
            lufs_ = r;
            repaint (lufsArea_);
        }
    }

    void updateMeter (const Engine::Meter& m)
    {
        auto fall = [] (float current, float incoming) { return incoming > current ? incoming : current * 0.88f; };
        peakL_ = fall (peakL_, m.peakL);
        peakR_ = fall (peakR_, m.peakR);
        rmsL_ = 0.8f * rmsL_ + 0.2f * m.rmsL;
        rmsR_ = 0.8f * rmsR_ + 0.2f * m.rmsR;
        repaint();
    }

    void resized() override
    {
        // deux colonnes pour tenir dans la hauteur d'une bande : réglages à gauche, fader et vu-mètres à droite
        auto r = getLocalBounds().reduced (3);
        rightArea_ = r.removeFromRight (50);
        r.removeFromRight (3);
        title_.setBounds (r.removeFromTop (18));
        output_.setBounds (r.removeFromTop (22).reduced (0, 1));
        panMode_.setBounds (r.removeFromTop (22).reduced (0, 1));
        inserts_.setBounds (r.removeFromTop (InsertList::heightNeeded() + 2).reduced (0, 1));
        // sonie : M (momentanée), S (court terme), I (intégrée), TP (crête vraie) puis le bouton de remise à zéro
        lufsArea_ = r.removeFromTop (4 * 13 + 2);
        lufsReset_.setBounds (r.removeFromTop (18).reduced (0, 1));
        r.removeFromTop (8);
        auto trimRow = r.removeFromTop (18);   // « Trim » et « Pan » : libellé à gauche, curseur à droite, sur la même ligne, sous la sonie
        trimLabel_.setBounds (trimRow.removeFromLeft (28));
        trim_.setBounds (trimRow);
        r.removeFromTop (2);
        auto panRow = r.removeFromTop (18);
        panLabel_.setBounds (panRow.removeFromLeft (28));
        pan_.setBounds (panRow);
        auto bottom = r.removeFromBottom (24);   // M et Mono en bas de la colonne
        mute_.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2).reduced (1));
        mono_.setBounds (bottom.reduced (1));
        auto col = rightArea_.withTrimmedBottom (16);
        faderArea_ = col;
        fader_.setBounds (col.removeFromLeft (22));
        meterArea_ = juce::Rectangle<int> (col.getX() + 1, col.getY(), 25, col.getHeight());   // 2 px de marge avant le bord du carré, plus les 3 px de marge de la bande : l'écart entre les deux mètres
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (owner_.colour (Token::BgCard));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 8.0f);
        g.setColour (owner_.colour (Token::Accent));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 8.0f, 1.5f);
        const juce::Rectangle<int> m = meterArea_;
        auto bar = [&] (float lin, float rms, int x)
        {
            const float db = dbOf (lin), rdb = dbOf (rms);
            const float h = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f) * static_cast<float> (m.getHeight());
            const float rh = juce::jlimit (0.0f, 1.0f, (rdb + 60.0f) / 66.0f) * static_cast<float> (m.getHeight());
            g.setColour (owner_.colour (Token::Grid));
            g.fillRoundedRectangle (static_cast<float> (x), static_cast<float> (m.getY()), 11.0f, static_cast<float> (m.getHeight()), 4.0f);
            const juce::Rectangle<float> level (static_cast<float> (x), static_cast<float> (m.getBottom()) - h, 11.0f, h);
            const auto top = db > 0.0f ? owner_.colour (Token::KnobHot) : owner_.colour (Token::Accent).brighter (0.3f);
            g.setGradientFill (juce::ColourGradient (top, level.getX(), level.getY(), owner_.colour (Token::Accent), level.getX(), level.getBottom(), false));
            g.fillRoundedRectangle (level, 4.0f);
            g.setColour (owner_.colour (Token::Text).withAlpha (0.6f)); // RMS
            g.fillRect (x + 2, m.getBottom() - static_cast<int> (rh), 6, static_cast<int> (rh));
        };
        bar (peakL_, rmsL_, m.getX());
        bar (peakR_, rmsR_, m.getX() + 14);
        {
            // LUFS : valeurs à une décimale, « -inf » en dessous de -100 ; la crête vraie passe en rouge au-dessus de -1 dBTP
            auto value = [] (float v) { return v <= -100.0f ? juce::String ("-inf") : juce::String (v, 1); };
            const char* const names[4] = { "M", "S", "I", "TP" };
            const float values[4] = { lufs_.momentary, lufs_.shortTerm, lufs_.integrated, lufs_.truePeakDb };
            g.setFont (juce::FontOptions (11.0f));
            for (int i = 0; i < 4; ++i)
            {
                auto row = juce::Rectangle<int> (lufsArea_.getX() + 2, lufsArea_.getY() + 1 + i * 13, lufsArea_.getWidth() - 4, 13);
                g.setColour (owner_.colour (Token::TextDim));
                g.drawText (names[i], row.removeFromLeft (22), juce::Justification::centredLeft);
                g.setColour (i == 3 && values[i] > -1.0f ? owner_.colour (Token::KnobHot) : owner_.colour (Token::Accent));
                g.drawText (value (values[i]) + (i == 3 ? " dB" : " LUFS"), row, juce::Justification::centredRight);
            }
        }
        g.setColour (owner_.colour (Token::Text));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (fader_.getValue() <= kFaderMinDb ? juce::String::fromUTF8 ("-inf") : juce::String (fader_.getValue(), 1) + " dB",
                    rightArea_.getX(), rightArea_.getBottom() - 14, rightArea_.getWidth(), 14, juce::Justification::centred);
    }

private:
    MixerView& owner_;
    juce::Label title_, trimLabel_, panLabel_;
    juce::ComboBox output_, panMode_;
    BoundSlider pan_, trim_, fader_;
    BoundToggle mute_, mono_;
    juce::Rectangle<int> faderArea_, lufsArea_, rightArea_, meterArea_;
    juce::TextButton lufsReset_;
    neptune::core::LoudnessMeter::Reading lufs_;
    float peakL_ = 0.0f, peakR_ = 0.0f, rmsL_ = 0.0f, rmsR_ = 0.0f;
    InsertList inserts_;
};

// ------------------------------------------------------------------------------------------------ la vue

MixerView::MixerView (NeptuneProcessor& p, const theme::Palette& palette, const i18n::Translator& tr)
    : processor_ (p), palette_ (palette), tr_ (tr)
{
    for (int t = 0; t < mixer::numTracks; ++t)
    {
        tracks_[static_cast<std::size_t> (t)] = std::make_unique<Strip> (*this, 0, t);
        trackRow_.addAndMakeVisible (*tracks_[static_cast<std::size_t> (t)]);
    }
    for (int q = 0; q < mixer::numParallels; ++q)
    {
        parallels_[static_cast<std::size_t> (q)] = std::make_unique<Strip> (*this, 2, q);
        trackRow_.addChildComponent (*parallels_[static_cast<std::size_t> (q)]);
    }
    trackViewport_.setViewedComponent (&trackRow_, false);
    trackViewport_.setScrollBarsShown (false, true);
    trackViewport_.setScrollBarThickness (12);
    addAndMakeVisible (trackViewport_);
    for (int b = 0; b < mixer::numBuses; ++b)
    {
        buses_[static_cast<std::size_t> (b)] = std::make_unique<Strip> (*this, 1, b);
        addAndMakeVisible (*buses_[static_cast<std::size_t> (b)]);
    }
    master_ = std::make_unique<MasterStrip> (*this);
    addAndMakeVisible (*master_);
    addTrack_.setButtonText (juce::String::fromUTF8 (std::string (tr.tr ("mix.add_track")).c_str()));
    addTrack_.onClick = [this]
    {
        if (processor_.uiAddTrack() >= 0) refresh();
    };
    trackRow_.addAndMakeVisible (addTrack_);
    tracksTitle_.setText (juce::String::fromUTF8 (std::string (tr.tr ("mix.tracks")).c_str()), juce::dontSendNotification);
    busesTitle_.setText (juce::String::fromUTF8 (std::string (tr.tr ("mix.buses")).c_str()), juce::dontSendNotification);
    for (auto* l : { &tracksTitle_, &busesTitle_ })
    {
        l->setText (l->getText().toUpperCase(), juce::dontSendNotification);
        l->setFont (juce::FontOptions (11.0f, juce::Font::bold));
        l->setColour (juce::Label::textColourId, colour (Token::TextDim2));
        addAndMakeVisible (*l);
    }
    zoom_ = juce::jlimit (0.6f, 1.6f, static_cast<float> (processor_.mixerZoomPercent()) / 100.0f);
    zoomOut_.setButtonText (juce::String::fromUTF8 ("\xe2\x88\x92"));
    zoomIn_.setButtonText ("+");
    zoomOut_.onClick = [this] { setZoom (zoom_ - 0.1f); };
    zoomIn_.onClick = [this] { setZoom (zoom_ + 0.1f); };
    zoomOut_.setTooltip (juce::String::fromUTF8 (std::string (tr.tr ("mix.zoom_tip")).c_str()));
    zoomIn_.setTooltip (zoomOut_.getTooltip());
    zoomLabel_.setJustificationType (juce::Justification::centred);
    zoomLabel_.setColour (juce::Label::textColourId, colour (Token::TextDim));
    for (auto* c : std::initializer_list<juce::Component*> { &zoomOut_, &zoomIn_, &zoomLabel_ }) addAndMakeVisible (*c);
    startTimerHz (30);
    refresh();
}

MixerView::~MixerView() { stopTimer(); }

juce::Colour MixerView::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

void MixerView::placeScaled (juce::Rectangle<int> area)
{
    lastArea_ = area;
    const float fit = juce::jmin (static_cast<float> (area.getWidth()) / designWidth, static_cast<float> (area.getHeight()) / designHeight);
    const float scale = juce::jmax (0.4f, juce::jmin (zoom_, fit));
    zoomLabel_.setText (juce::String (juce::roundToInt (scale * 100.0f)) + " %", juce::dontSendNotification);
    setTransform (juce::AffineTransform::scale (scale));
    // la transformation s'applique aux limites dans le repère du parent (autour de son origine) : position = zone / échelle
    setBounds (juce::roundToInt (static_cast<float> (area.getX()) / scale), juce::roundToInt (static_cast<float> (area.getY()) / scale),
               juce::roundToInt (static_cast<float> (area.getWidth()) / scale), juce::roundToInt (static_cast<float> (area.getHeight()) / scale));
}

void MixerView::setZoom (float z)
{
    zoom_ = juce::jlimit (0.6f, 1.6f, z);
    processor_.uiSetMixerZoomPercent (juce::roundToInt (zoom_ * 100.0f));
    if (! lastArea_.isEmpty()) placeScaled (lastArea_);
}

void MixerView::resized()
{
    layoutTracks();
}

// Parallèles visibles sous un parent : les filles d'une parallèle repliée sont masquées (elles restent actives dans le moteur).
void MixerView::appendParallels (int parentId, std::vector<int>& out) const
{
    for (const int p : processor_.parallelChildren (parentId))
    {
        out.push_back (p);
        if (expanded_[static_cast<std::size_t> (p)]) appendParallels (mixer::numTracks + p, out);
    }
}

void MixerView::toggleFold (int p)
{
    expanded_[static_cast<std::size_t> (p)] = ! expanded_[static_cast<std::size_t> (p)];
    refresh();
}

// Chaque piste suivie de ses pistes parallèles (à sa droite), le bouton « + Piste » à la fin ; bus et master à leur place.
// La rangée des pistes défile horizontalement si elle dépasse.
void MixerView::layoutTracks()
{
    auto r = getLocalBounds();
    const int scrollbar = 12;
    // deux conteneurs : « pistes » (titre, bandes qui défilent) puis « bus » (titre, 16 bus et le master à leur droite, de la même hauteur)
    const int chrome = 4 * kPanelPad + 2 * kPanelTitleH + kPanelGap + scrollbar;
    stripHeight_ = juce::jlimit (kMinStripH, kMaxStripH, (getHeight() - chrome) / 2);
    const int gap = 1;
    tracksPanel_ = r.removeFromTop (kPanelPad * 2 + kPanelTitleH + stripHeight_ + scrollbar);
    r.removeFromTop (kPanelGap);
    busesPanel_ = r.removeFromTop (kPanelPad * 2 + kPanelTitleH + stripHeight_);
    {
        auto inner = tracksPanel_.reduced (kPanelPad + 2, kPanelPad);
        // rangée du titre : « PISTES » à gauche, réglage du zoom (− pourcentage +) à droite
        auto top = inner.removeFromTop (kPanelTitleH);
        zoomIn_.setBounds (top.removeFromRight (26).reduced (0, 1));
        zoomLabel_.setBounds (top.removeFromRight (50));
        zoomOut_.setBounds (top.removeFromRight (26).reduced (0, 1));
        tracksTitle_.setBounds (top);
        auto area = inner;
        trackViewport_.setBounds (area);
        for (auto& s : parallels_) s->setVisible (false);
        int x = 0;
        for (int t = 0; t < mixer::numTracks; ++t)
        {
            auto& s = *tracks_[static_cast<std::size_t> (t)];
            if (! shownTracks_[static_cast<std::size_t> (t)]) continue;
            s.setBounds (x, 0, kStripW, stripHeight_);
            x += kStripW + gap;
            std::vector<int> kids;
            appendParallels (t, kids);
            for (const int p : kids)
            {
                auto& ps = *parallels_[static_cast<std::size_t> (p)];
                ps.setBounds (x, 0, kStripW, stripHeight_);
                ps.setVisible (true);
                x += kStripW + gap;
            }
        }
        addTrack_.setVisible (processor_.activeTrackCount() < mixer::numTracks);
        addTrack_.setBounds (x + 4, 4, kStripW + 20, 36);
        const int contentW = x + kStripW + 30;
        trackRow_.setSize (juce::jmax (contentW, area.getWidth() - 2), stripHeight_);
    }
    {
        auto inner = busesPanel_.reduced (kPanelPad + 2, kPanelPad);
        busesTitle_.setBounds (inner.removeFromTop (kPanelTitleH));
        int x = inner.getX();
        for (auto& s : buses_)
        {
            s->setBounds (x, inner.getY(), kStripW, stripHeight_);
            x += kStripW + gap;
        }
        master_->setBounds (x + 8, inner.getY(), kMasterW, stripHeight_);
    }
}

void MixerView::paint (juce::Graphics& g)
{
    for (const auto& r : { tracksPanel_, busesPanel_ })
    {
        if (r.isEmpty()) continue;
        const auto b = r.toFloat().reduced (0.5f);
        g.setColour (colour (Token::PanelBg));
        g.fillRoundedRectangle (b, 10.0f);
        g.setColour (colour (Token::Pb));
        g.drawRoundedRectangle (b, 10.0f, 1.0f);
    }
}

void MixerView::refresh()
{
    const int selected = processor_.selectedTrack();
    bool layoutChanged = false;
    for (int t = 0; t < mixer::numTracks; ++t)
    {
        const bool active = processor_.trackActive (t);
        auto& s = *tracks_[static_cast<std::size_t> (t)];
        if (active != shownTracks_[static_cast<std::size_t> (t)]) { shownTracks_[static_cast<std::size_t> (t)] = active; layoutChanged = true; }
        s.setVisible (active);
        if (active) s.refresh (t == selected);
    }
    for (int q = 0; q < mixer::numParallels; ++q)
    {
        const bool active = processor_.parallelActive (q);
        if (! active) expanded_[static_cast<std::size_t> (q)] = false;
        if (active != shownParallels_[static_cast<std::size_t> (q)]) { shownParallels_[static_cast<std::size_t> (q)] = active; layoutChanged = true; }
        if (active) parallels_[static_cast<std::size_t> (q)]->refresh (false);
    }
    // un parent ou un repliement a pu changer : on recalcule la disposition à chaque rafraîchissement (peu coûteux)
    for (int b = 0; b < mixer::numBuses; ++b) buses_[static_cast<std::size_t> (b)]->refresh (false);
    master_->refresh();
    layoutTracks();
    (void) layoutChanged;
}

void MixerView::timerCallback()
{
    if (! isShowing()) return;
    const auto rev = processor_.instrumentRevision();
    if (rev != lastRevision_)
    {
        lastRevision_ = rev;
        refresh();
    }
    for (int t = 0; t < mixer::numTracks; ++t)
    {
        if (! shownTracks_[static_cast<std::size_t> (t)]) continue;
        auto& s = *tracks_[static_cast<std::size_t> (t)];
        s.syncValues();
        s.updateMeter (processor_.trackMeter (t));
    }
    for (int q = 0; q < mixer::numParallels; ++q)
    {
        auto& s = *parallels_[static_cast<std::size_t> (q)];
        if (! s.isVisible()) continue;
        s.syncValues();
        s.updateMeter (processor_.parallelMeter (q));
    }
    for (int b = 0; b < mixer::numBuses; ++b)
    {
        auto& s = *buses_[static_cast<std::size_t> (b)];
        s.syncValues();
        s.updateMeter (processor_.busMeter (b));
    }
    master_->syncValues();
    master_->updateMeter (processor_.masterMeter());
    master_->updateLoudness (processor_.masterLoudness());
}

// Changement de générateur : la configuration d'instrument est perdue (spec §1), donc une confirmation si la piste en a une.
void MixerView::requestTypeChange (int track, TrackType type)
{
    if (processor_.trackType (track) == type) return;
    auto apply = [safe = juce::Component::SafePointer<MixerView> (this), track, type]
    {
        if (safe == nullptr) return;
        safe->processor_.uiSetTrackType (track, type);
        safe->refresh();
    };
    if (! processor_.trackHasContent (track)) { apply(); return; }
    auto text = [this] (const char* key) { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); };
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, text ("mix.confirm_title"),
                                                                                text ("mix.confirm_type"), text ("mix.continue"), text ("mix.cancel"), this),
                                  [apply] (int result) { if (result == 1) apply(); }); // 1 : premier bouton
}

void MixerView::requestExportTrack (int track)
{
    const auto name = juce::String::fromUTF8 (processor_.trackSettings (track).name.c_str());
    const auto title = u8 (std::string (tr_.tr ("track.export_title")));
    chooser_ = std::make_unique<juce::FileChooser> (title, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                               .getChildFile ((name.isEmpty() ? juce::String ("Track") : name) + ".neptunetrack"), "*.neptunetrack");
    chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                           [safe = juce::Component::SafePointer<MixerView> (this), track, title] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr || c.getResult() == juce::File()) return;
                               auto target = c.getResult();
                               if (! target.hasFileExtension ("neptunetrack")) target = target.withFileExtension ("neptunetrack");
                               const auto report = track_bundle::save (safe->processor_.exportTrackBundle (track), target);
                               const auto path = target.getFullPathName().toStdString();
                               const auto message = report.ok
                                   ? u8 (safe->tr_.format ("track.exported", { path, std::to_string (report.samples), std::to_string (report.images), std::to_string (report.packs), std::to_string (report.missing) }))
                                   : u8 (safe->tr_.format ("track.export_failed", { path }));
                               juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, message);
                           });
}

void MixerView::requestImportTrack()
{
    const auto title = u8 (std::string (tr_.tr ("track.import_title")));
    if (processor_.activeTrackCount() >= neptune::core::mixer::numTracks)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, u8 (std::string (tr_.tr ("track.import_full"))));
        return;
    }
    chooser_ = std::make_unique<juce::FileChooser> (title, juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.neptunetrack");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [safe = juce::Component::SafePointer<MixerView> (this), title] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr || ! c.getResult().existsAsFile()) return;
                               neptune::core::trackexport::Bundle bundle;
                               const auto assets = stylelib::directory().getParentDirectory().getChildFile ("imported-tracks");
                               const auto report = track_bundle::load (c.getResult(), assets, bundle);
                               if (! report.ok)
                               {
                                   juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title,
                                                                           u8 (safe->tr_.format ("track.import_failed", { report.error.toStdString() })));
                                   return;
                               }
                               const int index = safe->processor_.uiImportTrack (std::move (bundle));
                               if (index < 0)
                               {
                                   juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, u8 (std::string (safe->tr_.tr ("track.import_full"))));
                                   return;
                               }
                               safe->refresh();
                               juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title,
                                                                       u8 (safe->tr_.format ("track.imported", { std::to_string (index + 1), std::to_string (report.files), std::to_string (report.packsInstalled) })));
                           });
}

void MixerView::requestRemove (int track)
{
    if (processor_.activeTrackCount() <= 1) return; // le mixeur garde toujours une piste
    auto apply = [safe = juce::Component::SafePointer<MixerView> (this), track]
    {
        if (safe == nullptr) return;
        safe->processor_.uiRemoveTrack (track);
        safe->refresh();
    };
    if (! processor_.trackHasContent (track)) { apply(); return; }
    auto text = [this] (const char* key) { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); };
    const auto message = juce::String::fromUTF8 (tr_.format ("mix.confirm_remove", { std::to_string (track + 1) }).c_str());
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, text ("mix.confirm_title"),
                                                                                message, text ("mix.continue"), text ("mix.cancel"), this),
                                  [apply] (int result) { if (result == 1) apply(); });
}

// Ajout / retrait de pistes parallèles
void MixerView::requestAddParallel (int parentId)
{
    const int p = processor_.uiAddParallel (parentId);
    if (p >= 0)
    {
        if (parentId >= mixer::numTracks) expanded_[static_cast<std::size_t> (parentId - mixer::numTracks)] = true; // la nouvelle fille se voit
        refresh();
    }
}

void MixerView::requestRemoveParallel (int p)
{
    auto apply = [safe = juce::Component::SafePointer<MixerView> (this), p]
    {
        if (safe == nullptr) return;
        safe->processor_.uiRemoveParallel (p);
        safe->refresh();
    };
    const auto s = processor_.parallelSettings (p);
    bool hasInserts = false;
    for (int slot = 0; slot < mixer::numInserts; ++slot) hasInserts = hasInserts || ! processor_.insertType (NeptuneProcessor::StripKind::Parallel, p, slot).empty();
    if (processor_.parallelChildren (mixer::numTracks + p).empty() && ! hasInserts && s.strip.name.empty()) { apply(); return; }
    auto text = [this] (const char* key) { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); };
    const auto message = juce::String::fromUTF8 (tr_.format ("mix.confirm_remove_parallel", { std::to_string (p + 1) }).c_str());
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, text ("mix.confirm_title"),
                                                                                message, text ("mix.continue"), text ("mix.cancel"), this),
                                  [apply] (int result) { if (result == 1) apply(); });
}

// Solo exclusif (Maj+clic) : coupe les solos et les mutes des autres bandes du même domaine (pistes + parallèles, ou bus).
void MixerView::exclusiveSolo (int kind, int index)
{
    auto setParam = [this] (ParamId id, double v)
    {
        const auto* spec = processor_.parameters().find (id);
        if (spec == nullptr || processor_.parameters().get (id) == v) return;
        processor_.uiBeginEdit (id);
        processor_.uiSetNormalised (id, spec->toNormalized (v));
        processor_.uiEndEdit (id);
    };
    if (kind != 1)
    {
        for (int t = 0; t < mixer::numTracks; ++t)
            if (! (kind == 0 && t == index)) setParam (probe::trackParam (t, probe::kSolo), 0.0);
        for (int q = 0; q < mixer::numParallels; ++q)
            if (! (kind == 2 && q == index)) setParam (mixer::parallelParam (q, mixer::kParSolo), 0.0);
    }
    else
        for (int b = 0; b < mixer::numBuses; ++b)
        {
            if (b == index) continue;
            setParam (mixer::busParam (b, mixer::kBusSolo), 0.0);
        }
}
