#include "NovaOrganEditor.h"

#include <algorithm>
#include <cmath>

#include "neptune/core/Mixer.h"
#include "neptune/core/OrganPreset.h"
#include "neptune/core/SamplerParams.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace mx = neptune::core::mixer;

namespace
{
float dbOf (float lin) { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }

const char* const kFootage[9] = { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" };
// couleurs des tirettes : brun, brun, blanc, blanc, noir, blanc, noir, noir, blanc
const int kDrawbarKind[9] = { 0, 0, 1, 1, 2, 1, 2, 2, 1 };

juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0)   // tirettes
    {
        for (const float x : { 5.0f, 12.0f, 19.0f }) { p.startNewSubPath (x, 3.0f); p.lineTo (x, 21.0f); }
        p.addRectangle (3.0f, 8.0f, 4.0f, 3.0f);
        p.addRectangle (10.0f, 14.0f, 4.0f, 3.0f);
        p.addRectangle (17.0f, 5.0f, 4.0f, 3.0f);
    }
    else if (kind == 1)   // rotor
    {
        p.addEllipse (4.0f, 4.0f, 16.0f, 16.0f);
        p.startNewSubPath (12.0f, 12.0f); p.lineTo (18.0f, 8.0f);
    }
    else if (kind == 2)   // live : barres larges
    {
        p.addRectangle (4.0f, 4.0f, 6.0f, 16.0f);
        p.addRectangle (14.0f, 4.0f, 6.0f, 16.0f);
    }
    else if (kind == 3)   // matrice
    {
        for (const float x : { 4.0f, 14.0f }) for (const float y : { 4.0f, 14.0f }) p.addRectangle (x, y, 6.0f, 6.0f);
    }
    else if (kind == 5)   // orgue : roue
    {
        p.addEllipse (5.0f, 5.0f, 14.0f, 14.0f);
        p.addEllipse (10.0f, 10.0f, 4.0f, 4.0f);
        for (const float a : { 0.0f, 1.57f, 3.14f, 4.71f }) { p.startNewSubPath (12.0f + 7.0f * std::cos (a), 12.0f + 7.0f * std::sin (a)); p.lineTo (12.0f + 11.0f * std::cos (a), 12.0f + 11.0f * std::sin (a)); }
    }
    else   // réglages
    {
        for (const float y : { 6.0f, 12.0f, 18.0f }) { p.startNewSubPath (3.0f, y); p.lineTo (21.0f, y); }
        p.addEllipse (14.0f, 4.0f, 4.0f, 4.0f);
        p.addEllipse (6.0f, 10.0f, 4.0f, 4.0f);
        p.addEllipse (12.0f, 16.0f, 4.0f, 4.0f);
    }
    return p;
}
} // namespace

// ------------------------------------------------------------------------------------------------ tirette
class NovaOrganEditor::Drawbar : public juce::Component
{
public:
    Drawbar (const nova::Skin& s, ParamHost& h, ParamId id, int kind, juce::String footage) : skin_ (s), host_ (h), id_ (id), kind_ (kind), footage_ (std::move (footage)) {}

    void update()
    {
        const auto v = host_.parameters().version (id_);
        if (v == seen_) return;
        seen_ = v;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const juce::Colour body = kind_ == 0 ? juce::Colour (0xff6b3f1d) : kind_ == 1 ? juce::Colour (0xfff2eadb) : juce::Colour (0xff1c1a18);
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (10.0f));
        g.drawText (footage_, b.removeFromTop (skin_.px (16.0f)), juce::Justification::centred);
        const auto valueArea = b.removeFromBottom (skin_.px (18.0f));
        const auto slot = b.reduced (b.getWidth() * 0.3f, skin_.px (2.0f));
        g.setColour (skin_.colour (Token::Grid));
        g.fillRoundedRectangle (slot, skin_.px (3.0f));
        const int v = value();
        const float y = slot.getY() + slot.getHeight() * (static_cast<float> (v) / 8.0f);
        g.setColour (body);
        g.fillRoundedRectangle (b.getX() + skin_.px (3.0f), slot.getY(), b.getWidth() - skin_.px (6.0f), std::max (0.0f, y - slot.getY()), skin_.px (3.0f));
        g.setColour (skin_.colour (Token::Accent));
        g.fillRoundedRectangle (b.getX() + skin_.px (1.0f), y - skin_.px (6.0f), b.getWidth() - skin_.px (2.0f), skin_.px (12.0f), skin_.px (3.0f));
        g.setColour (skin_.colour (Token::Text));
        g.setFont (skin_.font (12.0f, true));
        g.drawText (juce::String (v), valueArea, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent& e) override { host_.uiBeginEdit (id_); set (e); }
    void mouseDrag (const juce::MouseEvent& e) override { set (e); }
    void mouseUp (const juce::MouseEvent&) override { host_.uiEndEdit (id_); }
    void mouseDoubleClick (const juce::MouseEvent&) override { host_.uiResetToDefault (id_); repaint(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int next = std::clamp (value() + (w.deltaY > 0 ? -1 : 1), 0, 8);   // vers le haut : tirette rentrée (valeur plus faible)
        nova::setParamNormalised (host_, id_, next / 8.0);
        repaint();
    }
    int value() const { return std::clamp (static_cast<int> (std::lround (host_.parameters().get (id_))), 0, 8); }

private:
    void set (const juce::MouseEvent& e)
    {
        const auto b = getLocalBounds().toFloat();
        const float top = b.getY() + skin_.px (18.0f), h = b.getHeight() - skin_.px (36.0f);
        const float p = std::clamp ((e.position.y - top) / std::max (1.0f, h), 0.0f, 1.0f);
        host_.uiSetNormalised (id_, static_cast<double> (std::lround (p * 8.0f)) / 8.0);
        repaint();
    }
    const nova::Skin& skin_;
    ParamHost& host_;
    ParamId id_;
    int kind_;
    juce::String footage_;
    std::uint32_t seen_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ vu-mètre de contrôle d'un canal MIDI (vélocité d'entrée)
class NovaOrganEditor::MidiBar : public juce::Component
{
public:
    // output : barre de niveau de sortie (montée immédiate, retombée rapide, sans numéro de canal) ; sinon barre MIDI (hauteur = vélocité, aucune animation)
    MidiBar (const nova::Skin& s, juce::String letter, bool output = false) : skin_ (s), letter_ (std::move (letter)), output_ (output) {}

    // level : 0..1 ; channel : canal MIDI du clavier (0 = désactivé), sans objet pour une barre de sortie
    void update (float level, int channel)
    {
        float next = level;
        if (output_) next = std::max (level, shown_ * 0.9f);   // le maximum évite de vibrer quand le niveau est stable
        if (next < 0.004f) next = 0.0f;
        if (next == shown_ && channel == channel_) return;
        shown_ = next;
        channel_ = channel;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto track = b.withTrimmedTop (skin_.px (2.0f)).withTrimmedBottom (skin_.px (34.0f)).withSizeKeepingCentre (skin_.px (14.0f), b.getHeight() - skin_.px (36.0f));
        skin_.fill (g, Token::StageBg, track, skin_.px (4.0f));
        if ((output_ || channel_ > 0) && shown_ > 0.0f)
        {
            const float h = track.getHeight() * shown_;
            g.setGradientFill (juce::ColourGradient (skin_.colour (Token::KnobHot), 0.0f, track.getY(), skin_.colour (Token::Accent), 0.0f, track.getBottom(), false));
            g.fillRoundedRectangle (track.withTop (track.getBottom() - h).reduced (skin_.px (1.0f), 0.0f), skin_.px (3.0f));
        }
        g.setColour (skin_.colour (Token::Text));
        g.setFont (skin_.font (12.0f, true));
        g.drawText (letter_, juce::Rectangle<float> (b.getX(), b.getBottom() - skin_.px (34.0f), b.getWidth(), skin_.px (16.0f)), juce::Justification::centred);
        if (output_) return;
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (10.0f));
        g.drawText (channel_ > 0 ? juce::String (channel_) : "-", juce::Rectangle<float> (b.getX(), b.getBottom() - skin_.px (16.0f), b.getWidth(), skin_.px (14.0f)), juce::Justification::centred);
    }

private:
    const nova::Skin& skin_;
    juce::String letter_;
    bool output_;
    float shown_ = 0.0f;
    int channel_ = -1;
};

// ------------------------------------------------------------------------------------------------ curseur d'un champ de la description de l'orgue
class NovaOrganEditor::DescSlider : public juce::Component
{
public:
    DescSlider (const nova::Skin& s, juce::String title, double lo, double hi, double step, juce::String suffix, std::function<double()> get, std::function<void (double)> set,
                bool commitOnRelease, double defaultValue)
        : skin_ (s), title_ (std::move (title)), lo_ (lo), hi_ (hi), step_ (step), suffix_ (std::move (suffix)), get_ (std::move (get)), set_ (std::move (set)),
          commitOnRelease_ (commitOnRelease), default_ (defaultValue) {}

    void update()
    {
        if (dragging_) return;
        const double v = get_();
        if (v == shown_) return;
        shown_ = v;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (11.0f, true));
        g.drawText (title_, b.withHeight (skin_.px (15.0f)), juce::Justification::centredLeft, true);
        const auto t = track();
        g.setColour (skin_.colour (Token::Grid));
        g.fillRoundedRectangle (t, t.getHeight() / 2);
        const float x = t.getX() + static_cast<float> ((shown_ - lo_) / (hi_ - lo_)) * t.getWidth();
        g.setColour (skin_.colour (Token::Accent));
        g.fillRoundedRectangle (t.withRight (std::max (t.getX() + 1.0f, x)), t.getHeight() / 2);
        const float hd = skin_.px (12.0f);
        g.setColour (skin_.colour (Token::Hring));
        g.fillEllipse (x - hd / 2 - 2.0f, t.getCentreY() - hd / 2 - 2.0f, hd + 4.0f, hd + 4.0f);
        g.setColour (skin_.colour (Token::Handle));
        g.fillEllipse (x - hd / 2, t.getCentreY() - hd / 2, hd, hd);
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (12.0f));
        const int decimals = step_ >= 1.0 ? 0 : step_ >= 0.1 ? 1 : 2;
        g.drawText (juce::String (shown_, decimals) + suffix_, b.withTop (t.getBottom() + skin_.px (3.0f)).withHeight (skin_.px (16.0f)), juce::Justification::centredLeft, true);
    }
    void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) { dragging_ = true; applyFromX (e.position.x); } }
    void mouseDrag (const juce::MouseEvent& e) override { if (dragging_) applyFromX (e.position.x); }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging_) return;
        dragging_ = false;
        if (commitOnRelease_) set_ (shown_);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { shown_ = default_; set_ (default_); repaint(); }

private:
    juce::Rectangle<float> track() const
    {
        const auto b = getLocalBounds().toFloat();
        return { b.getX() + skin_.px (6.0f), b.getY() + skin_.px (22.0f), b.getWidth() - skin_.px (12.0f), skin_.px (6.0f) };
    }
    void applyFromX (float x)
    {
        const auto t = track();
        double v = lo_ + std::clamp (static_cast<double> ((x - t.getX()) / t.getWidth()), 0.0, 1.0) * (hi_ - lo_);
        if (step_ > 0.0) v = lo_ + std::round ((v - lo_) / step_) * step_;
        shown_ = std::clamp (v, lo_, hi_);
        if (! commitOnRelease_) set_ (shown_);
        repaint();
    }
    const nova::Skin& skin_;
    juce::String title_;
    double lo_, hi_, step_;
    juce::String suffix_;
    std::function<double()> get_;
    std::function<void (double)> set_;
    bool commitOnRelease_;
    double default_;
    double shown_ = -1.0e30;
    bool dragging_ = false;
};

// ------------------------------------------------------------------------------------------------ pastille de valeur entière glissable (splits)
class NovaOrganEditor::NumPill : public juce::Component
{
public:
    NumPill (const nova::Skin& s, juce::String label, int lo, int hi, std::function<int()> get, std::function<void (int)> set, std::function<juce::String (int)> text)
        : skin_ (s), label_ (std::move (label)), lo_ (lo), hi_ (hi), get_ (std::move (get)), set_ (std::move (set)), text_ (std::move (text)) {}

    void update()
    {
        if (dragging_) return;
        const int v = get_();
        if (v == shown_) return;
        shown_ = v;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        skin_.fill (g, Token::StageBg, b, skin_.px (8.0f));
        g.setColour (skin_.colour (Token::Grid));
        g.drawRoundedRectangle (b, skin_.px (8.0f), skin_.px (1.0f));
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (10.0f, true));
        g.drawText (label_, b.reduced (skin_.px (8.0f), 0.0f), juce::Justification::centredLeft, true);
        g.setColour (skin_.colour (Token::Text));
        g.setFont (skin_.font (13.0f, true));
        g.drawText (text_ ? text_ (shown_) : juce::String (shown_), b.reduced (skin_.px (8.0f), 0.0f), juce::Justification::centredRight, true);
    }
    void mouseDown (const juce::MouseEvent& e) override { dragging_ = ! e.mods.isPopupMenu(); start_ = shown_; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging_) return;
        const int steps = static_cast<int> (std::lround (-static_cast<float> (e.getDistanceFromDragStartY()) / (skin_.px (8.0f) * (e.mods.isShiftDown() ? 4.0f : 1.0f))));
        const int v = std::clamp (start_ + steps, lo_, hi_);
        if (v != shown_) { shown_ = v; set_ (v); repaint(); }
    }
    void mouseUp (const juce::MouseEvent&) override { dragging_ = false; }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int v = std::clamp (get_() + (w.deltaY > 0 ? 1 : -1), lo_, hi_);
        shown_ = v;
        set_ (v);
        repaint();
    }

private:
    const nova::Skin& skin_;
    juce::String label_;
    int lo_, hi_, shown_ = -1000, start_ = 0;
    std::function<int()> get_;
    std::function<void (int)> set_;
    std::function<juce::String (int)> text_;
    bool dragging_ = false;
};

// ------------------------------------------------------------------------------------------------ pastille à menu (canaux, transpositions, câblage)
struct NovaOrganEditor::MenuPill
{
    nova::Pill* pill = nullptr;
    std::function<juce::String()> label;
    std::vector<juce::String> items;
    std::function<int()> get;
    std::function<void (int)> set;
    juce::String shown;
};

// ------------------------------------------------------------------------------------------------ grille 9 tirettes x 5 plages d'octave (câblage étendu)
class NovaOrganEditor::TaperGrid : public juce::Component
{
public:
    using Table = std::array<std::array<int, 5>, 9>;
    TaperGrid (const nova::Skin& s, std::function<Table()> get, std::function<void (const Table&)> set, std::function<bool()> enabled, std::vector<juce::String> rangeNames)
        : skin_ (s), get_ (std::move (get)), set_ (std::move (set)), enabled_ (std::move (enabled)), rangeNames_ (std::move (rangeNames)) {}

    void update()
    {
        if (dragging_) return;
        const auto t = get_();
        const bool en = enabled_();
        if (t == shown_ && en == shownEnabled_) return;
        shown_ = t;
        shownEnabled_ = en;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        skin_.fill (g, Token::StageBg, getLocalBounds().toFloat(), skin_.px (6.0f));
        g.setFont (skin_.font (10.0f, true));
        g.setColour (skin_.colour (Token::TextDim2));
        for (int c = 0; c < 9; ++c) g.drawText (kFootage[c], cell (-1, c), juce::Justification::centred);
        for (int r = 0; r < 5; ++r)
        {
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (rangeNames_[static_cast<std::size_t> (r)], rangeLabel (r), juce::Justification::centredRight);
            for (int c = 0; c < 9; ++c)
            {
                const int v = shown_[static_cast<std::size_t> (c)][static_cast<std::size_t> (r)];
                const auto rc = cell (r, c).reduced (skin_.px (2.0f));
                const float amount = std::clamp (static_cast<float> (v) / 24.0f, -1.0f, 1.0f);
                g.setColour (skin_.colour (v == 0 ? Token::Grid : (v > 0 ? Token::Accent : Token::Pink), shownEnabled_ ? (0.25f + 0.5f * std::fabs (amount)) : 0.15f));
                g.fillRoundedRectangle (rc, skin_.px (4.0f));
                g.setColour (skin_.colour (v == 0 ? Token::TextDim2 : Token::Text, shownEnabled_ ? 1.0f : 0.4f));
                g.setFont (skin_.font (11.0f, v != 0));
                g.drawText (v > 0 ? "+" + juce::String (v) : juce::String (v), rc, juce::Justification::centred);
            }
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        active_ = {};
        if (! shownEnabled_ || e.mods.isPopupMenu()) return;
        for (int r = 0; r < 5; ++r) for (int c = 0; c < 9; ++c) if (cell (r, c).contains (e.position)) active_ = { r, c };
        if (active_.row >= 0) { start_ = shown_[static_cast<std::size_t> (active_.col)][static_cast<std::size_t> (active_.row)]; dragging_ = true; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging_ || active_.row < 0) return;
        const int steps = static_cast<int> (std::lround (-static_cast<float> (e.getDistanceFromDragStartY()) / (skin_.px (4.0f) * (e.mods.isShiftDown() ? 4.0f : 1.0f))));
        shown_[static_cast<std::size_t> (active_.col)][static_cast<std::size_t> (active_.row)] = std::clamp (start_ + steps, -24, 12);
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging_) return;
        dragging_ = false;
        set_ (shown_);   // chaque changement reconstruit les roues : appliqué à la fin du geste seulement
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! shownEnabled_) return;
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 9; ++c)
                if (cell (r, c).contains (e.position)) { shown_[static_cast<std::size_t> (c)][static_cast<std::size_t> (r)] = 0; set_ (shown_); repaint(); }
    }

private:
    struct At { int row = -1, col = -1; };
    juce::Rectangle<float> cell (int row, int col) const
    {
        const float left = skin_.px (90.0f), top = skin_.px (22.0f);
        const float w = (static_cast<float> (getWidth()) - left - skin_.px (8.0f)) / 9.0f, h = (static_cast<float> (getHeight()) - top - skin_.px (6.0f)) / 5.0f;
        return { left + static_cast<float> (col) * w, row < 0 ? 0.0f : top + static_cast<float> (row) * h, w, row < 0 ? top : h };
    }
    juce::Rectangle<float> rangeLabel (int row) const { const auto c = cell (row, 0); return { skin_.px (4.0f), c.getY(), skin_.px (82.0f), c.getHeight() }; }

    const nova::Skin& skin_;
    std::function<Table()> get_;
    std::function<void (const Table&)> set_;
    std::function<bool()> enabled_;
    std::vector<juce::String> rangeNames_;
    Table shown_ {};
    bool shownEnabled_ = true, dragging_ = false;
    At active_;
    int start_ = 0;
};

// ------------------------------------------------------------------------------------------------ ensembles de commandes (une page chacun)
struct NovaOrganEditor::DrawbarSet
{
    std::vector<Drawbar*> bars;     // 9 supérieur, 9 inférieur, 2 pédalier
    std::vector<nova::Text*> titles; // UPPER, LOWER, PEDAL
};
struct NovaOrganEditor::ToggleSet
{
    std::vector<nova::Pill*> pills;  // percussion x4, vibrato supérieur et inférieur, drive
    nova::Pill* vibMode = nullptr;
};
struct NovaOrganEditor::LeslieSet
{
    std::vector<nova::Pill*> squares; // chorale, tremolo, frein
};

NovaOrganEditor::DrawbarSet* NovaOrganEditor::makeDrawbars (int page)
{
    auto* set = new DrawbarSet;
    owned_sets_.emplace_back (set);
    auto make = [&] (int offset, int kind, const char* footage) { set->bars.push_back (add<Drawbar> (page, skin(), host(), mx::organParam (track_, offset), kind, juce::String (footage))); };
    for (int i = 0; i < 9; ++i) make (mx::kOrgUpper0 + i, kDrawbarKind[i], kFootage[i]);
    for (int i = 0; i < 9; ++i) make (mx::kOrgLower0 + i, kDrawbarKind[i], kFootage[i]);
    make (mx::kOrgPedal16, 0, "16'");
    make (mx::kOrgPedal8, 1, "8'");
    for (const char* key : { "organ.upper", "organ.lower", "organ.pedal" })
    {
        auto* t = add<nova::Text> (page, skin(), Token::TextDim2, 11.0f, true);
        t->setText (tx (key).toUpperCase());
        set->titles.push_back (t);
    }
    return set;
}

NovaOrganEditor::ToggleSet* NovaOrganEditor::makeToggles (int page, bool large)
{
    auto* set = new ToggleSet;
    owned_toggles_.emplace_back (set);
    const auto look = large ? nova::Pill::Look::Pill : nova::Pill::Look::Small;
    struct T { int kind; const char* key; };
    for (const T t : { T { mx::kOrgPercOn, "organ.perc_on" }, T { mx::kOrgPercSoft, "organ.perc_soft" }, T { mx::kOrgPercFast, "organ.perc_fast" }, T { mx::kOrgPercThird, "organ.perc_third" },
                       T { mx::kOrgVibUpper, "organ.vib_upper" }, T { mx::kOrgVibLower, "organ.vib_lower" } })
    {
        auto* p = add<nova::Pill> (page, skin(), look);
        p->bindToggle (host(), mx::organParam (track_, t.kind), tx (t.key));
        set->pills.push_back (p);
    }
    set->vibMode = add<nova::Pill> (page, skin(), look);
    const auto id = mx::organParam (track_, mx::kOrgVibMode);
    std::vector<juce::String> choices;
    if (const auto* spec = host().parameters().find (id)) for (const auto& c : spec->choices) choices.push_back (juce::String::fromUTF8 (c.c_str()));
    set->vibMode->bindChoice (host(), id, tx ("organ.vibrato"), choices);
    return set;
}

NovaOrganEditor::LeslieSet* NovaOrganEditor::makeLeslie (int page)
{
    auto* set = new LeslieSet;
    owned_leslie_.emplace_back (set);
    const char* keys[3] = { "organ.leslie_chorale", "organ.leslie_tremolo", "organ.leslie_brake" };
    for (int v = 0; v < 3; ++v)
    {
        auto* p = add<nova::Pill> (page, skin());
        p->setText (tx (keys[v]));
        p->onClick = [this, v] { nova::setParamNormalised (host(), mx::organParam (track_, mx::kOrgLeslieSpeed), static_cast<double> (v) / 2.0); };
        set->squares.push_back (p);
    }
    return set;
}

NovaOrganEditor::MenuPill* NovaOrganEditor::makeMenuPill (int page, std::function<juce::String()> label, std::vector<juce::String> items, std::function<int()> get, std::function<void (int)> set)
{
    auto* m = new MenuPill;
    owned_menus_.emplace_back (m);
    m->pill = add<nova::Pill> (page, skin(), nova::Pill::Look::Small);
    m->label = std::move (label);
    m->items = std::move (items);
    m->get = std::move (get);
    m->set = std::move (set);
    m->pill->onClick = [this, m]
    {
        juce::PopupMenu menu;
        for (std::size_t i = 0; i < m->items.size(); ++i) menu.addItem (static_cast<int> (i) + 1, m->items[i], true, static_cast<int> (i) == m->get());
        juce::Component::SafePointer<juce::Component> safe (this);
        menu.setLookAndFeel (&m->pill->getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (m->pill), [m, safe] (int r) { if (safe != nullptr && r > 0) m->set (r - 1); });
    };
    return m;
}

void NovaOrganEditor::editDesc (const std::function<void (organ::OrganDescription&)>& change)
{
    auto d = desc_;
    change (d);
    if (d == desc_) return;
    desc_ = d;
    link_.edit (d);
}

// ------------------------------------------------------------------------------------------------ éditeur
NovaOrganEditor::NovaOrganEditor (ParamHost& host, PresetHost& presets, OrganLink link, const theme::Palette& palette, theme::KnobStyle knob, const i18n::Translator& tr, Options options)
    : EditorBase (host, palette, knob, tr, std::move (options), juce::String::fromUTF8 ("ORGAMON")), link_ (std::move (link)), presets_ (presets), track_ (link_.track)
{
    desc_ = link_.description();
    ui_ = std::make_unique<nova::PresetUi> (*this, presets_, palette, tr, [this] { desc_ = link_.description(); });
    addPage (tx ("organ.page_drawbars"), iconPath (0));
    addPage (tx ("organ.page_leslie"), iconPath (1));
    addPage (tx ("organ.page_live"), iconPath (2));
    addPage (tx ("og.page_matrix"), iconPath (3));
    addPage (tx ("og.page_organ"), iconPath (5));
    addPage (tx ("nv.page_settings"), iconPath (4));
    buildLeftColumn();
    buildDrawbarsPage();
    buildLeslieAndLivePages();
    buildMatrixPage();
    buildSettingsExtras();
    buildSettingsPage (5);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaOrganEditor::~NovaOrganEditor() = default;

// ------------------------------------------------------------------------------------------------ colonne de gauche
void NovaOrganEditor::buildLeftColumn()
{
    metersPanel_ = add<nova::Panel> (-1, skin());
    metersPanel_->setTitle ("MIDI / OUT");
    lesliePanel_ = add<nova::Panel> (-1, skin());
    lesliePanel_->setTitle (tx ("organ.leslie").toUpperCase());
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();   // sous les entrées du menu, créées avant lui
    for (const char* letter : { "L", "R" }) outBars_.push_back (add<MidiBar> (-1, skin(), juce::String (letter), true));
    for (const char* letter : { "U", "L", "P" }) midiBars_.push_back (add<MidiBar> (-1, skin(), juce::String (letter)));
    notesText_ = add<nova::Text> (-1, skin(), Token::TextDim2, 11.0f, true);
    leftLeslie_ = makeLeslie (-1);
    addInfo (-1, metersPanel_, "hlp.og_meters", true);
    addInfo (-1, lesliePanel_, "hlp.og_leslie", true);
}

// ------------------------------------------------------------------------------------------------ page DRAWBARS
void NovaOrganEditor::buildDrawbarsPage()
{
    drawbarsPanel_ = add<nova::Panel> (0, skin());
    drawbarsPanel_->setTitle (tx ("og.drawbars").toUpperCase());
    drawbars_ = makeDrawbars (0);
    percPanel_ = add<nova::Panel> (0, skin());
    percPanel_->setTitle (tx ("organ.percussion").toUpperCase());
    vibPanel_ = add<nova::Panel> (0, skin());
    vibPanel_->setTitle (tx ("organ.vibrato").toUpperCase());
    manualsPanel_ = add<nova::Panel> (0, skin());
    manualsPanel_->setTitle (tx ("organ.keyboards").toUpperCase());
    toggles_ = makeToggles (0, false);
    addInfo (0, drawbarsPanel_, "hlp.og_drawbars", true);
    addInfo (0, percPanel_, "hlp.og_percussion", true);
    addInfo (0, vibPanel_, "hlp.og_vibrato", true);
    addInfo (0, manualsPanel_, "hlp.og_manuals", true);

    // canaux MIDI et transpositions des trois claviers
    const char* names[3] = { "U", "L", "P" };
    std::vector<juce::String> channels { tx ("organ.off") };
    for (int c = 1; c <= 16; ++c) channels.push_back (juce::String (c));
    std::vector<juce::String> transposes;
    for (int t = -24; t <= 24; ++t) transposes.push_back ((t > 0 ? "+" : "") + juce::String (t));
    for (int m = 0; m < 3; ++m)
    {
        const juce::String name = names[m];
        auto* ch = makeMenuPill (0, [this, name] { return tx ("organ.channel").substring (0, 2).toUpperCase() + " " + name; }, channels,
                                 [this, m] { return desc_.channel[static_cast<std::size_t> (m)]; },
                                 [this, m] (int v) { editDesc ([m, v] (organ::OrganDescription& d) { d.channel[static_cast<std::size_t> (m)] = v; }); });
        manualMenus_.push_back (ch);
    }
    for (int m = 0; m < 3; ++m)
    {
        const juce::String name = names[m];
        auto* tr = makeMenuPill (0, [name] { return "TR " + name; }, transposes,
                                 [this, m] { return desc_.transpose[static_cast<std::size_t> (m)] + 24; },
                                 [this, m] (int v) { editDesc ([m, v] (organ::OrganDescription& d) { d.transpose[static_cast<std::size_t> (m)] = v - 24; }); });
        manualMenus_.push_back (tr);
    }

    keyboardPanel_ = add<nova::Panel> (0, skin());
    keyboard_ = add<SynthKeyboard> (0, skin().palette());
    auto channelOfUpper = [this] { return std::max (0, desc_.channel[0] - 1); };
    keyboard_->onNoteOn = [this, channelOfUpper] (int key, float) { link_.noteOn (channelOfUpper(), key); };
    keyboard_->onNoteOff = [this, channelOfUpper] (int key) { link_.noteOff (channelOfUpper(), key); };
    octDown_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
    octDown_->setText (tx ("synth.oct_down"));
    octDown_->onClick = [this] { keyboard_->shiftOctave (-1); };
    octUp_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
    octUp_->setText (tx ("synth.oct_up"));
    octUp_->onClick = [this] { keyboard_->shiftOctave (1); };
    holdPill_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
    holdPill_->setText (tx ("synth.hold"));
    holdPill_->onClick = [this] { holdPill_->setOn (! holdPill_->isOn()); keyboard_->setHold (holdPill_->isOn()); };
    // points de split : note MIDI sous laquelle un clavier plus bas prend le relais
    auto noteText = [] (int note) { static const char* const n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; return juce::String (n[note % 12]) + juce::String (note / 12 - 1) + " (" + juce::String (note) + ")"; };
    numPills_.push_back (add<NumPill> (0, skin(), "SPLIT P/L", 0, 127, [this] { return desc_.splitLower; },
                                       [this] (int v) { editDesc ([v] (organ::OrganDescription& d) { d.splitLower = v; }); }, noteText));
    numPills_.push_back (add<NumPill> (0, skin(), "SPLIT L/U", 0, 127, [this] { return desc_.splitUpper; },
                                       [this] (int v) { editDesc ([v] (organ::OrganDescription& d) { d.splitUpper = v; }); }, noteText));
    addInfo (0, numPills_[0], "hlp.og_split");

    // potards : vibrato (profondeur), overdrive (3), Leslie (mélange), réverbération, puis volume et gain du plugin
    struct Spec { ParamId id; const char* title; bool pink; const char* help; };
    const Spec specs[8] = { { mx::organParam (track_, mx::kOrgVibDepth), "og.vib_depth", false, "hlp.og_vibdepth" }, { mx::organParam (track_, mx::kOrgDriveInput), "og.drive_in", false, "hlp.og_drive" },
                            { mx::organParam (track_, mx::kOrgDriveCharacter), "og.drive_char", false, "hlp.og_drivechar" }, { mx::organParam (track_, mx::kOrgDriveOutput), "og.drive_out", false, "hlp.og_driveout" },
                            { mx::organParam (track_, mx::kOrgLeslieMix), "og.leslie_mix", false, "hlp.og_lesliemix" }, { mx::organParam (track_, mx::kOrgReverb), "og.reverb", false, "hlp.og_reverb" },
                            { link_.volumeId, "og.volume", true, "hlp.og_volume" }, { link_.gainId, "og.gain", true, "hlp.og_gain" } };
    for (const auto& s : specs)
    {
        Column c {};
        c.card = add<nova::Panel> (0, skin());
        c.knob = add<nova::KnobCard> (0, skin(), host(), s.id, tx (s.title), modalOpener(), s.pink);
        c.knob->setPlain (true);
        addInfo (0, c.card, s.help, 1);
        columns_.push_back (c);
    }
    drivePill_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Small);
    drivePill_->bindToggle (host(), mx::organParam (track_, mx::kOrgDriveOn), "ON");
}

// ------------------------------------------------------------------------------------------------ pages LESLIE et LIVE
void NovaOrganEditor::buildLeslieAndLivePages()
{
    // LESLIE
    lesliePagePanel_ = add<nova::Panel> (1, skin());
    lesliePagePanel_->setTitle (tx ("organ.leslie").toUpperCase());
    leslieSet_ = makeLeslie (1);
    leslieMixCard_ = add<nova::KnobCard> (1, skin(), host(), mx::organParam (track_, mx::kOrgLeslieMix), tx ("og.leslie_mix"), modalOpener(), false);
    struct S { const char* group; const char* key; double organ::OrganDescription::* field; double lo, hi, step; const char* suffix; double def; };
    const S defs[] = {
        { "organ.horn", "organ.horn_slow", &organ::OrganDescription::hornSlowRpm, 5.0, 200.0, 0.5, " rpm", 48.0 },
        { "organ.horn", "organ.horn_fast", &organ::OrganDescription::hornFastRpm, 100.0, 900.0, 1.0, " rpm", 400.0 },
        { "organ.horn", "organ.horn_level", &organ::OrganDescription::hornLevel, 0.0, 1.0, 0.01, "", 0.5 },
        { "organ.drum", "organ.drum_slow", &organ::OrganDescription::drumSlowRpm, 5.0, 100.0, 0.5, " rpm", 40.0 },
        { "organ.drum", "organ.drum_fast", &organ::OrganDescription::drumFastRpm, 60.0, 600.0, 1.0, " rpm", 342.0 },
        { "organ.horn", "organ.horn_acc", &organ::OrganDescription::hornAcc, 0.05, 2.0, 0.01, " s", 0.161 },
        { "organ.horn", "organ.horn_dec", &organ::OrganDescription::hornDec, 0.05, 2.0, 0.01, " s", 0.321 },
        { "organ.drum", "organ.drum_acc", &organ::OrganDescription::drumAcc, 0.5, 10.0, 0.05, " s", 4.127 },
        { "organ.drum", "organ.drum_dec", &organ::OrganDescription::drumDec, 0.5, 10.0, 0.05, " s", 1.371 },
    };
    const organ::OrganDescription defaults;
    for (const auto& d : defs)
    {
        auto field = d.field;
        auto* sl = add<DescSlider> (1, skin(), tx (d.group) + " - " + tx (d.key), d.lo, d.hi, d.step, juce::String (d.suffix),
                                    [this, field] { return desc_.*field; },
                                    [this, field] (double v) { editDesc ([field, v] (organ::OrganDescription& x) { x.*field = v; }); }, false, defaults.*(d.field));
        addInfo (1, sl, d.field == &organ::OrganDescription::hornLevel ? "hlp.og_hornlevel" : (d.field == &organ::OrganDescription::hornAcc || d.field == &organ::OrganDescription::hornDec || d.field == &organ::OrganDescription::drumAcc || d.field == &organ::OrganDescription::drumDec) ? "hlp.og_leslietime" : "hlp.og_leslierpm", 2);
        leslieSliders_.push_back (sl);
    }
    // LIVE
    livePanel_ = add<nova::Panel> (2, skin());
    livePanel_->setTitle (tx ("organ.page_live").toUpperCase());
    liveDrawbars_ = makeDrawbars (2);
    liveLowerPanel_ = add<nova::Panel> (2, skin());
    liveToggles_ = makeToggles (2, true);
    liveLeslie_ = makeLeslie (2);
}

// ------------------------------------------------------------------------------------------------ page MATRICE
void NovaOrganEditor::buildMatrixPage()
{
    matrixPanel_ = add<nova::Panel> (3, skin());
    matrixPanel_->setTitle (tx ("og.matrix_title").toUpperCase());
    tiles_ = add<nova::PresetTiles> (3, skin(), presets_);
    tiles_->onLoaded = [this] { desc_ = link_.description(); };
    addInfo (3, matrixPanel_, "hlp.og_matrix", true);
    bankPrev_ = add<nova::Pill> (3, skin(), nova::Pill::Look::Small);
    bankPrev_->setText ("<");
    bankPrev_->onClick = [this] { tiles_->bank = (tiles_->bank + preset::numBanks - 1) % preset::numBanks; };
    bankNext_ = add<nova::Pill> (3, skin(), nova::Pill::Look::Small);
    bankNext_->setText (">");
    bankNext_->onClick = [this] { tiles_->bank = (tiles_->bank + 1) % preset::numBanks; };
    bankPill_ = add<nova::Pill> (3, skin());
    bankPill_->setStatic (true);
    colsPill_ = add<nova::Pill> (3, skin());
    colsPill_->onClick = [this] { tiles_->cols = tiles_->cols % 8 + 1; };
    rowsPill_ = add<nova::Pill> (3, skin());
    rowsPill_->onClick = [this] { tiles_->rows = tiles_->rows % 8 + 1; };
    fieldPill_ = add<nova::Pill> (3, skin());
    fieldPill_->onClick = [this] { tiles_->field = (tiles_->field + 1) % 5; };
    addInfo (3, fieldPill_, "hlp.og_field");
    tiles_->textLoad = tx ("sy.tile_load");
    tiles_->textAssign = tx ("sy.tile_assign");
    tiles_->textClear = tx ("sy.tile_clear");
    fieldsTitle_ = add<nova::Text> (3, skin(), Token::TextDim2, 11.0f, true);
    fieldsTitle_->setText (tx ("sy.fields_title").toUpperCase());
    fields_ = add<nova::PresetFields> (3, skin(), presets_, translator());
    addInfo (3, fieldsTitle_, "hlp.sy_fields");
}

// ------------------------------------------------------------------------------------------------ page ORGUE (accord, clic des touches, câblage, MIDI)
void NovaOrganEditor::buildSettingsExtras()
{
    constexpr int page = 4;
    lesliePanel_->toBack();   // (ordre d'empilement sans conséquence : les panneaux sont des fonds)
    auto* panel = add<nova::Panel> (page, skin());
    panel->setTitle (tx ("og.organ_title").toUpperCase());
    tuneSection_ = add<nova::Text> (page, skin(), Token::TextDim2, 11.0f, true);
    tuneSection_->setText (tx ("organ.tune_section").toUpperCase());
    clickSection_ = add<nova::Text> (page, skin(), Token::TextDim2, 11.0f, true);
    clickSection_->setText (tx ("organ.click_section").toUpperCase());
    wiringSection_ = add<nova::Text> (page, skin(), Token::TextDim2, 11.0f, true);
    wiringSection_->setText (tx ("organ.wiring").toUpperCase());
    midiText_ = add<nova::Text> (page, skin(), Token::TextDim, 11.0f);
    midiText_->setText (tx ("organ.midi_hint"));
    organ_settings_panel_ = panel;

    addInfo (page, panel, "hlp.og_organpage", true);
    settingsSliders_.push_back (add<DescSlider> (page, skin(), tx ("organ.tune"), -100.0, 100.0, 1.0, " cents", [this] { return static_cast<double> (desc_.tuningCents); },
                                                 [this] (double v) { editDesc ([v] (organ::OrganDescription& d) { d.tuningCents = static_cast<int> (v); }); }, true, 0.0));
    settingsSliders_.push_back (add<DescSlider> (page, skin(), tx ("organ.click_level"), 0.0, 100.0, 1.0, " %", [this] { return static_cast<double> (desc_.clickLevel); },
                                                 [this] (double v) { editDesc ([v] (organ::OrganDescription& d) { d.clickLevel = static_cast<int> (v); }); }, true, 50.0));
    settingsSliders_.push_back (add<DescSlider> (page, skin(), tx ("organ.click_length"), 25.0, 400.0, 5.0, " %", [this] { return static_cast<double> (desc_.clickLength); },
                                                 [this] (double v) { editDesc ([v] (organ::OrganDescription& d) { d.clickLength = static_cast<int> (v); }); }, true, 100.0));
    settingsSliders_.push_back (add<DescSlider> (page, skin(), tx ("organ.click_release"), 0.0, 100.0, 1.0, " %", [this] { return static_cast<double> (desc_.clickRelease); },
                                                 [this] (double v) { editDesc ([v] (organ::OrganDescription& d) { d.clickRelease = static_cast<int> (v); }); }, true, 0.0));
    addInfo (page, settingsSliders_[0], "hlp.og_tune", 2);
    for (std::size_t i = 1; i < settingsSliders_.size(); ++i) addInfo (page, settingsSliders_[i], "hlp.og_click", 2);
    wiringMode_ = makeMenuPill (page, [this] { return tx ("organ.wiring"); }, { tx ("organ.wiring_classic"), tx ("organ.wiring_extended") },
                                [this] { return desc_.extended ? 1 : 0; }, [this] (int v) { editDesc ([v] (organ::OrganDescription& d) { d.extended = v == 1; }); });
    foldbackPill_ = add<nova::Pill> (page, skin());
    addInfo (page, wiringMode_->pill, "hlp.og_wiring");
    foldbackPill_->onClick = [this] { editDesc ([] (organ::OrganDescription& d) { d.foldback = ! d.foldback; }); };
    taperGrid_ = add<TaperGrid> (page, skin(),
                                 [this] { TaperGrid::Table t {}; for (std::size_t b = 0; b < 9; ++b) for (std::size_t k = 0; k < 5; ++k) t[b][k] = desc_.taperDb[b][k]; return t; },
                                 [this] (const TaperGrid::Table& t) { editDesc ([&t] (organ::OrganDescription& d) { for (std::size_t b = 0; b < 9; ++b) for (std::size_t k = 0; k < 5; ++k) d.taperDb[b][k] = t[b][k]; }); },
                                 [this] { return desc_.extended; },
                                 std::vector<juce::String> { "C0-B1", "C2-B2", "C3-B3", "C4-B4", "C5+" });
    ccPill_ = add<nova::Pill> (page, skin());
    ccPill_->onClick = [this] { link_.setCc (! link_.ccEnabled()); };
    addInfo (page, ccPill_, "hlp.og_cc");
    if (link_.carteMenu)
    {
        cartePill_ = add<nova::Pill> (page, skin());
        cartePill_->onClick = [this] { link_.carteMenu (cartePill_); };
    }
    panicPill_ = add<nova::Pill> (page, skin());
    panicPill_->setText (tx ("panic.button"));
    panicPill_->onClick = [this] { link_.panic(); keyboard_->releaseAll(); };
}

// ------------------------------------------------------------------------------------------------ presets : menu, enregistrement, gestionnaire
void NovaOrganEditor::showPresetMenu (juce::Component* anchor) { ui_->showMenu (anchor, tiles_ != nullptr ? tiles_->bank : 0); }

bool NovaOrganEditor::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    if (m.isCommandDown() && (k.getKeyCode() == 'S' || k.getKeyCode() == 's')) { ui_->quickSave(); return true; }
    return EditorBase::keyPressed (k);
}

// ------------------------------------------------------------------------------------------------ mise en page
void NovaOrganEditor::layout()
{
    // ---- colonne de gauche
    metersPanel_->setBounds (R (14, 72, 176, 222));
    for (std::size_t i = 0; i < midiBars_.size(); ++i) midiBars_[i]->setBounds (R (22.0f + static_cast<float> (i) * 28.0f, 72 + 32, 26, 156));
    for (std::size_t i = 0; i < outBars_.size(); ++i) outBars_[i]->setBounds (R (22.0f + 3.0f * 28.0f + 10.0f + static_cast<float> (i) * 28.0f, 72 + 32, 26, 156));
    notesText_->setBounds (R (14 + 8, 72 + 188, 160, 24));
    lesliePanel_->setBounds (R (14, 306, 176, 176));
    for (std::size_t i = 0; i < leftLeslie_->squares.size(); ++i) leftLeslie_->squares[i]->setBounds (R (24, 340.0f + 46.0f * static_cast<float> (i), 156, 40));
    navPanel_->setBounds (R (14, 496, 176, 214));
    layoutNav (496, 214);

    // ---- placement d'un jeu de tirettes : supérieur, inférieur, pédalier
    auto placeBars = [this] (DrawbarSet& set, float y, float h)
    {
        const float w = 38.0f, gap = 3.0f;
        const float xs[3] = { 220.0f, 610.0f, 1000.0f };
        for (int block = 0; block < 3; ++block)
        {
            const int count = block == 2 ? 2 : 9, first = block == 0 ? 0 : block == 1 ? 9 : 18;
            for (int i = 0; i < count; ++i) set.bars[static_cast<std::size_t> (first + i)]->setBounds (R (xs[block] + static_cast<float> (i) * (w + gap), y, w, h));
            set.titles[static_cast<std::size_t> (block)]->setBounds (R (xs[block], y - 22.0f, 200, 16));
        }
    };

    // ---- page DRAWBARS
    drawbarsPanel_->setBounds (R (204, 72, 942, 222));
    placeBars (*drawbars_, 124, 160);
    percPanel_->setBounds (R (204, 306, 248, 98));
    for (std::size_t i = 0; i < 4; ++i) toggles_->pills[i]->setBounds (R (216.0f + static_cast<float> (i % 2) * 118.0f, 336.0f + static_cast<float> (i / 2) * 32.0f, 112, 28));
    vibPanel_->setBounds (R (464, 306, 248, 98));
    toggles_->vibMode->setBounds (R (476, 336, 224, 28));
    toggles_->pills[4]->setBounds (R (476, 368, 108, 28));
    toggles_->pills[5]->setBounds (R (592, 368, 108, 28));
    manualsPanel_->setBounds (R (724, 306, 422, 98));
    for (std::size_t i = 0; i < manualMenus_.size(); ++i)
        manualMenus_[i]->pill->setBounds (R (736.0f + static_cast<float> (i % 3) * 136.0f, 336.0f + static_cast<float> (i / 3) * 32.0f, 128, 28));
    keyboardPanel_->setBounds (R (204, 416, 942, 66));
    keyboard_->setBounds (R (216, 420, 690, 58));
    octDown_->setBounds (R (918, 420, 66, 26));
    octUp_->setBounds (R (990, 420, 66, 26));
    holdPill_->setBounds (R (1062, 420, 74, 26));
    numPills_[0]->setBounds (R (918, 450, 108, 28));
    numPills_[1]->setBounds (R (1030, 450, 108, 28));
    const float colW = (942.0f - 12.0f * 7.0f) / 8.0f;
    for (std::size_t i = 0; i < columns_.size(); ++i)
    {
        const float x = 204.0f + static_cast<float> (i) * (colW + 12.0f);
        columns_[i].card->setBounds (R (x, 496, colW, 214));
        columns_[i].knob->setBounds (R (x, 496, colW, 132));
    }
    drivePill_->setBounds (R (204.0f + (colW + 12.0f) + colW - 46.0f, 504, 40, 22));

    // ---- page LESLIE
    lesliePagePanel_->setBounds (R (204, 72, 942, 638));
    for (std::size_t i = 0; i < leslieSet_->squares.size(); ++i) leslieSet_->squares[i]->setBounds (R (220.0f + static_cast<float> (i) * 242.0f, 108, 230, 130));
    leslieMixCard_->setBounds (R (950, 100, 180, 150));
    for (std::size_t i = 0; i < leslieSliders_.size(); ++i)
        leslieSliders_[i]->setBounds (R (220.0f + static_cast<float> (i % 3) * 310.0f, 290.0f + static_cast<float> (i / 3) * 84.0f, 290, 64));

    // ---- page LIVE
    livePanel_->setBounds (R (204, 72, 942, 322));
    placeBars (*liveDrawbars_, 124, 250);
    liveLowerPanel_->setBounds (R (204, 406, 942, 304));
    for (std::size_t i = 0; i < 4; ++i) liveToggles_->pills[i]->setBounds (R (220.0f + static_cast<float> (i % 2) * 144.0f, 430.0f + static_cast<float> (i / 2) * 60.0f, 136, 50));
    liveToggles_->vibMode->setBounds (R (220, 560, 280, 46));
    liveToggles_->pills[4]->setBounds (R (220, 614, 136, 46));
    liveToggles_->pills[5]->setBounds (R (364, 614, 136, 46));
    for (std::size_t i = 0; i < liveLeslie_->squares.size(); ++i) liveLeslie_->squares[i]->setBounds (R (540.0f + static_cast<float> (i) * 200.0f, 430, 190, 230));

    // ---- page MATRICE
    matrixPanel_->setBounds (R (204, 72, 942, 638));
    bankPrev_->setBounds (R (220, 108, 36, 30));
    bankPill_->setBounds (R (260, 108, 150, 30));
    bankNext_->setBounds (R (414, 108, 36, 30));
    colsPill_->setBounds (R (470, 108, 150, 30));
    rowsPill_->setBounds (R (630, 108, 150, 30));
    fieldPill_->setBounds (R (790, 108, 340, 30));
    tiles_->setBounds (R (220, 152, 640, 540));
    fieldsTitle_->setBounds (R (880, 152, 240, 16));
    fields_->setBounds (R (880, 172, 250, 520));

    // ---- page ORGUE
    organ_settings_panel_->setBounds (R (204, 72, 942, 638));
    tuneSection_->setBounds (R (220, 104, 300, 16));
    settingsSliders_[0]->setBounds (R (220, 124, 300, 64));
    clickSection_->setBounds (R (220, 206, 300, 16));
    for (std::size_t i = 1; i < settingsSliders_.size(); ++i) settingsSliders_[i]->setBounds (R (220, 226.0f + static_cast<float> (i - 1) * 72.0f, 300, 64));
    wiringSection_->setBounds (R (560, 104, 400, 16));
    wiringMode_->pill->setBounds (R (560, 126, 300, 32));
    foldbackPill_->setBounds (R (876, 126, 254, 32));
    taperGrid_->setBounds (R (560, 176, 570, 260));
    ccPill_->setBounds (R (560, 462, 270, 34));
    panicPill_->setBounds (R (850, 462, 200, 34));
    if (cartePill_ != nullptr) cartePill_->setBounds (R (560, 504, 490, 34));
    midiText_->setBounds (R (220, 540, 910, 80));

    // ---- barre du bas, en-tête, gestionnaire
    abPill()->setBounds (R (14, 724 + 12, 40, 32));
    presetPill()->setBounds (R (440, 11, 280, 36));
    ui_->resized();
}

void NovaOrganEditor::tick()
{
    if (link_.collect) link_.collect();   // libère l'orgue remplacé après un changement d'accord ou de câblage
    const auto rev = link_.undoRevision();
    if (rev != lastUndoRevision_)
    {
        lastUndoRevision_ = rev;
        desc_ = link_.description();
    }
    presetPill()->setText (ui_->label());
    for (std::size_t c = 0; c < outBars_.size(); ++c) outBars_[c]->update (std::clamp ((dbOf (link_.outPeak (static_cast<int> (c))) + 60.0f) / 60.0f, 0.0f, 1.0f), 0);   // -60 dBFS en bas, 0 dBFS en haut
    for (std::size_t m = 0; m < midiBars_.size(); ++m)
    {
        const int ch = desc_.channel[m];   // 0 : clavier désactivé, sinon canal 1..16
        midiBars_[m]->update (ch > 0 ? link_.midiLevel (ch - 1) : 0.0f, ch);
    }
    notesText_->setText (juce::String (link_.heldCount()) + " " + tx ("og.notes"));
    const int speed = choiceValue (mx::organParam (track_, mx::kOrgLeslieSpeed));
    for (auto* set : { leftLeslie_, leslieSet_, liveLeslie_ })
        for (std::size_t i = 0; i < set->squares.size(); ++i) set->squares[i]->setOn (static_cast<int> (i) == speed);
    const int page = currentPage();
    if (page == 0)
    {
        for (auto* b : drawbars_->bars) b->update();
        for (auto* m : manualMenus_) { const auto t = m->label() + "  " + m->items[static_cast<std::size_t> (std::clamp (m->get(), 0, static_cast<int> (m->items.size()) - 1))]; if (t != m->shown) { m->shown = t; m->pill->setText (t); } }
        for (auto* n : numPills_) n->update();
        keyboard_->setHeldKeys (link_.heldWord (0), link_.heldWord (1));
        holdPill_->setOn (holdPill_->isOn());
    }
    if (page == 1) for (auto* s : leslieSliders_) s->update();
    if (page == 2) for (auto* b : liveDrawbars_->bars) b->update();
    if (page == 3)
    {
        tiles_->update();
        fields_->update();
        bankPill_->setText (tx ("og.bank") + " " + juce::String::fromUTF8 (preset::defaultBankName (tiles_->bank).c_str()) + " (" + juce::String (tiles_->bank) + ")");
        colsPill_->setText (tx ("og.columns") + "  " + juce::String (tiles_->cols));
        rowsPill_->setText (tx ("og.rows") + "  " + juce::String (tiles_->rows));
        static const char* const fields[5] = { "og.f_preset", "og.f_song", "og.f_workshop", "og.f_student", "og.f_event" };
        fieldPill_->setText (tx ("og.main_field") + ": " + tx (fields[tiles_->field]));
    }
    if (page == 4)
    {
        for (auto* s : settingsSliders_) s->update();
        taperGrid_->update();
        const auto t = wiringMode_->label() + "  " + wiringMode_->items[desc_.extended ? 1 : 0];
        if (t != wiringMode_->shown) { wiringMode_->shown = t; wiringMode_->pill->setText (t); }
        foldbackPill_->setText (tx ("organ.foldback"));
        foldbackPill_->setOn (desc_.foldback);
        foldbackPill_->setEnabled (desc_.extended);
        foldbackPill_->setAlpha (desc_.extended ? 1.0f : 0.4f);
        ccPill_->setText (tx ("control.button") + "  " + (link_.ccEnabled() ? "ON" : "OFF"));
        ccPill_->setOn (link_.ccEnabled());
        if (cartePill_ != nullptr && link_.carteText)
        {
            const auto t = link_.carteText();
            if (t != carteShown_) { carteShown_ = t; cartePill_->setText (t); }
            cartePill_->setOn (t.startsWith (juce::String::fromUTF8 ("Carte : ON")));
        }
    }
}

void NovaOrganEditor::paintBackground (juce::Graphics& g)
{
    g.setColour (skin().colour (Token::TextDim2));
    g.setFont (skin().font (11.0f, true));
    g.drawText (ui_->statusText, R (730, 724 + 12, 416, 32).toFloat(), juce::Justification::centredRight);
}
