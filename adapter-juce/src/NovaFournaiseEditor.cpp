#include "NovaFournaiseEditor.h"
#include "neptune/core/OverdrivePresets.h"

#include <algorithm>
#include <cmath>

using namespace neptune::core;
using neptune::core::theme::Token;
namespace ovd = neptune::core::overdrive;

namespace
{
float dbOf (float lin) { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }

void setPlain (ParamHost& host, ParamId id, double v)
{
    if (const auto* spec = host.parameters().find (id)) host.uiSetNormalised (id, spec->toNormalized (v));
}

juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0)   // courbe de saturation
    {
        p.startNewSubPath (3.0f, 19.0f);
        p.cubicTo (9.0f, 19.0f, 8.0f, 5.0f, 12.0f, 5.0f);
        p.lineTo (21.0f, 5.0f);
    }
    else if (kind == 1)   // filtres : passe-haut puis passe-bas
    {
        p.startNewSubPath (3.0f, 19.0f); p.lineTo (9.0f, 8.0f); p.lineTo (21.0f, 8.0f);
    }
    else if (kind == 2)   // égaliseur : trois points sur une courbe
    {
        for (const float x : { 6.0f, 12.0f, 18.0f }) p.addEllipse (x - 2.0f, (x == 12.0f ? 6.0f : 14.0f) - 2.0f, 4.0f, 4.0f);
        p.startNewSubPath (3.0f, 14.0f); p.lineTo (21.0f, 14.0f);
    }
    else if (kind == 3)   // entrée latérale : une flèche qui entre
    {
        p.startNewSubPath (3.0f, 12.0f); p.lineTo (17.0f, 12.0f);
        p.startNewSubPath (12.0f, 6.0f); p.lineTo (18.0f, 12.0f); p.lineTo (12.0f, 18.0f);
    }
    else
    {
        p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
        p.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
    }
    return p;
}

constexpr float kPillFont = 13.0f;
const juce::Colour kGreen (0xFF3DBE5E), kRed (0xFFE5484D);
} // namespace

// ------------------------------------------------------------------------------------------------ rangée de pastilles à choix unique
class NovaFournaiseEditor::ChipRow : public juce::Component, public nova::Refreshable
{
public:
    ChipRow (const nova::Skin& s, ParamHost& h, ParamId id, std::vector<juce::String> labels) : skin_ (s), host_ (h), id_ (id), labels_ (std::move (labels)) {}

    void refresh() override
    {
        const auto v = host_.parameters().version (id_);
        if (v == seen_) return;
        seen_ = v;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const int sel = static_cast<int> (std::lround (host_.parameters().get (id_)));
        for (int i = 0; i < static_cast<int> (labels_.size()); ++i)
        {
            const auto r = chip (i);
            const bool on = i == sel;
            if (on) skin_.fillActive (g, r, skin_.px (8.0f));
            else skin_.fill (g, Token::StageBg, r, skin_.px (8.0f));
            g.setColour (skin_.colour (on ? Token::Accent : Token::Border));
            g.drawRoundedRectangle (r.reduced (skin_.px (0.5f)), skin_.px (8.0f), std::max (1.0f, skin_.borderWidth()));
            g.setColour (skin_.colour (on ? Token::Text : Token::TextDim));
            g.setFont (skin_.font (14.0f, on));
            g.drawText (labels_[static_cast<std::size_t> (i)], r, juce::Justification::centred);
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < static_cast<int> (labels_.size()); ++i)
            if (chip (i).contains (e.position)) { nova::setParam (host_, id_, i); repaint(); return; }
    }

private:
    juce::Rectangle<float> chip (int i) const
    {
        const int n = static_cast<int> (labels_.size());
        const float gap = skin_.px (8.0f), w = (static_cast<float> (getWidth()) - gap * static_cast<float> (n - 1)) / static_cast<float> (n);
        return { static_cast<float> (i) * (w + gap), 0.0f, w, static_cast<float> (getHeight()) };
    }
    const nova::Skin& skin_;
    ParamHost& host_;
    ParamId id_;
    std::vector<juce::String> labels_;
    std::uint32_t seen_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ courbe de transfert du modèle choisi (avec le niveau d'entrée du moment)
class NovaFournaiseEditor::TransferView : public juce::Component
{
public:
    TransferView (const nova::Skin& s, ParamHost& h, const OverdriveModule* m, const i18n::Translator& tr) : skin_ (s), host_ (h), module_ (m), tr_ (tr) {}

    void update()
    {
        const auto& ps = host_.parameters();
        std::uint32_t key = 0;
        for (const ParamId id : { ovd::model, ovd::bias, ovd::character, ovd::feel, ovd::drive }) key = key * 31u + ps.version (id);
        const float peak = std::max (module_->meter (ovd::kInPeakL), module_->meter (ovd::kInPeakR));
        live_ = std::max (peak, live_ * 0.85f);
        const auto liveKey = static_cast<std::uint32_t> (juce::roundToInt (std::min (live_, 8.0f) * 200.0f));
        if (key == key_ && liveKey == liveKey_) return;
        key_ = key;
        liveKey_ = liveKey;
        valid_ = false;
        repaint();
    }
    void resized() override { valid_ = false; }

    void paint (juce::Graphics& g) override
    {
        skin_.fill (g, Token::StageBg, getLocalBounds().toFloat(), skin_.px (8.0f));
        const auto plot = plotArea();
        g.setFont (skin_.font (10.0f));
        for (int i = -3; i <= 3; ++i)
        {
            g.setColour (skin_.colour (Token::Grid, i == 0 ? 0.9f : 0.5f));
            g.drawVerticalLine (juce::roundToInt (xOf (i)), plot.getY(), plot.getBottom());
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (juce::String (i), juce::Rectangle<float> (xOf (i) - skin_.px (12.0f), plot.getBottom() + skin_.px (3.0f), skin_.px (24.0f), skin_.px (12.0f)), juce::Justification::centred);
        }
        for (const float v : { -1.0f, 0.0f, 1.0f })
        {
            g.setColour (skin_.colour (Token::Grid, v == 0.0f ? 0.9f : 0.5f));
            g.drawHorizontalLine (juce::roundToInt (yOf (v)), plot.getX(), plot.getRight());
        }
        if (! valid_) rebuild();
        // référence : droite de pente 1 (sans saturation)
        g.setColour (skin_.colour (Token::TextDim2, 0.5f));
        g.drawLine (xOf (-1.3), yOf (-1.3), xOf (1.3), yOf (1.3), std::max (1.0f, skin_.px (1.0f)));
        g.setColour (skin_.colour (Token::Accent, 0.18f));
        g.fillPath (area_);
        g.setColour (skin_.colour (Token::Accent));
        g.strokePath (curve_, juce::PathStrokeType (skin_.px (2.4f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // point du moment : le pic d'entrée, après le gain de drive
        const double driveLin = std::pow (10.0, host_.parameters().get (ovd::drive) / 20.0);
        const double x = std::clamp (static_cast<double> (live_) * driveLin, 0.0, 3.0);
        const double y = ovd::shape (x, shapeParams());
        g.setColour (skin_.colour (Token::Pink));
        const float r = skin_.px (5.0f);
        g.fillEllipse (xOf (x) - r, yOf (y) - r, r * 2.0f, r * 2.0f);
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (10.0f, true));
        g.drawText (juce::String::fromUTF8 (std::string (tr_.tr ("fo.transfer_axes")).c_str()), juce::Rectangle<float> (plot.getX() + skin_.px (6.0f), plot.getY() + skin_.px (4.0f), plot.getWidth(), skin_.px (14.0f)), juce::Justification::centredLeft);
    }

private:
    ovd::ShapeParams shapeParams() const
    {
        const auto& ps = host_.parameters();
        ovd::ShapeParams p;
        p.model = std::clamp (static_cast<int> (std::lround (ps.get (ovd::model))), 0, ovd::kNumModels - 1);
        p.bias = ps.get (ovd::bias) / 100.0;
        p.character = ps.get (ovd::character) / 100.0;
        p.feel = ps.get (ovd::feel) / 100.0;
        return p;
    }
    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (skin_.px (10.0f)).withTrimmedRight (skin_.px (10.0f)).withTrimmedTop (skin_.px (10.0f)).withTrimmedBottom (skin_.px (24.0f));
    }
    float xOf (double v) const { const auto p = plotArea(); return p.getCentreX() + static_cast<float> (std::clamp (v, -3.0, 3.0) / 3.0) * p.getWidth() * 0.5f; }
    float yOf (double v) const { const auto p = plotArea(); return p.getCentreY() - static_cast<float> (std::clamp (v, -1.35, 1.35) / 1.35) * p.getHeight() * 0.5f; }
    void rebuild()
    {
        curve_.clear();
        area_.clear();
        const auto sp = shapeParams();
        const int n = std::max (8, static_cast<int> (plotArea().getWidth() / 2.0f));
        for (int i = 0; i <= n; ++i)
        {
            const double x = -3.0 + 6.0 * static_cast<double> (i) / n;
            const float px = xOf (x), py = yOf (ovd::shape (x, sp));
            if (i == 0) curve_.startNewSubPath (px, py); else curve_.lineTo (px, py);
        }
        area_ = curve_;
        area_.lineTo (xOf (3.0), yOf (0.0));
        area_.lineTo (xOf (-3.0), yOf (0.0));
        area_.closeSubPath();
        valid_ = true;
    }

    const nova::Skin& skin_;
    ParamHost& host_;
    const OverdriveModule* module_;
    const i18n::Translator& tr_;
    juce::Path curve_, area_;
    bool valid_ = false;
    float live_ = 0.0f;
    std::uint32_t key_ = 0xFFFFFFFFu, liveKey_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ niveau de la clé et modulation du drive qui en découle
class NovaFournaiseEditor::KeyView : public juce::Component
{
public:
    KeyView (const nova::Skin& s, ParamHost& h, const OverdriveModule* m, const i18n::Translator& tr) : skin_ (s), host_ (h), module_ (m), tr_ (tr) {}

    void update()
    {
        const float k = module_->meter (ovd::kKeyLevel);
        level_ = std::max (k, level_ * 0.9f);
        const auto key = static_cast<std::uint32_t> (juce::roundToInt (level_ * 400.0f)) * 31u + host_.parameters().version (ovd::keyAmount);
        if (key == key_) return;
        key_ = key;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        skin_.fill (g, Token::StageBg, getLocalBounds().toFloat(), skin_.px (8.0f));
        const auto bar = getLocalBounds().toFloat().reduced (skin_.px (16.0f), skin_.px (30.0f));
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (10.0f, true));
        g.drawText (juce::String::fromUTF8 (std::string (tr_.tr ("fo.key_level")).c_str()), juce::Rectangle<float> (bar.getX(), skin_.px (8.0f), bar.getWidth(), skin_.px (16.0f)), juce::Justification::centredLeft);
        g.setColour (skin_.colour (Token::Grid, 0.5f));
        g.fillRoundedRectangle (bar, skin_.px (4.0f));
        g.setGradientFill (juce::ColourGradient (kGreen, bar.getX(), 0.0f, kRed, bar.getRight(), 0.0f, false));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * std::clamp (level_, 0.0f, 1.0f)), skin_.px (4.0f));
        const double mod = host_.parameters().get (ovd::keyAmount) / 100.0 * 30.0 * static_cast<double> (level_);
        g.setColour (skin_.colour (Token::Text));
        g.setFont (skin_.font (14.0f, true));
        g.drawText (juce::String::fromUTF8 (std::string (tr_.tr ("fo.drive_mod")).c_str()) + "  " + juce::String (mod, 1) + " dB", juce::Rectangle<float> (bar.getX(), bar.getBottom() + skin_.px (6.0f), bar.getWidth(), skin_.px (18.0f)), juce::Justification::centredLeft);
    }

private:
    const nova::Skin& skin_;
    ParamHost& host_;
    const OverdriveModule* module_;
    const i18n::Translator& tr_;
    float level_ = 0.0f;
    std::uint32_t key_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ réponse de l'égaliseur (même vue que celle de Convolution)
class NovaFournaiseEditor::EqView : public juce::Component
{
public:
    EqView (const nova::Skin& s, ParamHost& h, const i18n::Translator& tr, std::function<double()> rate) : skin_ (s), host_ (h), tr_ (tr), rate_ (std::move (rate)) {}

    void update()
    {
        std::uint32_t key = 0;
        for (ParamId id = ovd::eqOn; id <= ovd::eqLpSlope; ++id) key = key * 31u + host_.parameters().version (id);
        if (key == key_) return;
        key_ = key;
        valid_ = false;
        repaint();
    }

    void resized() override { valid_ = false; }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        skin_.fill (g, Token::StageBg, b, skin_.px (6.0f));
        const auto plot = plotArea();
        g.setFont (skin_.font (9.0f));
        for (const double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            g.setColour (skin_.colour (Token::Grid, 0.5f));
            g.drawVerticalLine (juce::roundToInt (xOf (f)), plot.getY(), plot.getBottom());
            if (f == 100.0 || f == 1000.0 || f == 10000.0 || f == 20.0 || f == 20000.0)
            {
                g.setColour (skin_.colour (Token::TextDim2));
                g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String (f, 0),
                            juce::Rectangle<float> (xOf (f) - skin_.px (18.0f), plot.getBottom() + skin_.px (3.0f), skin_.px (36.0f), skin_.px (12.0f)), juce::Justification::centred);
            }
        }
        for (int d = -18; d <= 18; d += 6)
        {
            g.setColour (skin_.colour (Token::Grid, d == 0 ? 0.9f : 0.5f));
            g.drawHorizontalLine (juce::roundToInt (yOf (d)), plot.getX(), plot.getRight());
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (juce::String (d), juce::Rectangle<float> (b.getX() + skin_.px (2.0f), yOf (d) - skin_.px (6.0f), plot.getX() - b.getX() - skin_.px (6.0f), skin_.px (12.0f)), juce::Justification::centredRight);
        }
        if (! valid_) rebuild();
        const bool eqOn = host_.parameters().get (ovd::eqOn) >= 0.5;
        juce::Path filled (total_);
        filled.lineTo (plot.getRight(), yOf (0.0));
        filled.lineTo (plot.getX(), yOf (0.0));
        filled.closeSubPath();
        g.setColour (skin_.colour (Token::Accent, eqOn ? 0.12f : 0.04f));
        g.fillPath (filled);
        g.setColour (skin_.colour (Token::Accent, eqOn ? 1.0f : 0.35f));
        g.strokePath (total_, juce::PathStrokeType (skin_.px (2.2f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        for (int i = 0; i < kHandles; ++i)
        {
            const auto pt = pointOf (i);
            const bool on = host_.parameters().get (onId (i)) >= 0.5, active = i == drag_ || (drag_ < 0 && i == hover_);
            const float r = skin_.px (active ? 7.0f : 5.5f);
            g.setColour (skin_.colour (colourFor (i), on ? 1.0f : 0.5f));
            if (on) g.fillEllipse (pt.x - r, pt.y - r, r * 2, r * 2);
            else g.drawEllipse (pt.x - r, pt.y - r, r * 2, r * 2, skin_.px (1.6f));
            if (on)
            {
                g.setColour (skin_.colour (Token::StageBg));
                g.drawEllipse (pt.x - r, pt.y - r, r * 2, r * 2, skin_.px (1.2f));
            }
        }
        const int shown = drag_ >= 0 ? drag_ : hover_;
        if (shown >= 0)
        {
            g.setColour (skin_.colour (colourFor (shown)));
            g.setFont (skin_.font (11.0f, true));
            g.drawText (describe (shown), juce::Rectangle<float> (plot.getX() + skin_.px (8.0f), plot.getY() + skin_.px (4.0f), plot.getWidth() - skin_.px (16.0f), skin_.px (16.0f)), juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h != hover_) { hover_ = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseExit (const juce::MouseEvent&) override { if (hover_ >= 0) { hover_ = -1; repaint(); } }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h < 0 || e.mods.isPopupMenu()) return;
        drag_ = h;
        if (host_.parameters().get (onId (h)) < 0.5)   // saisir une poignée coupée la met en service
        {
            host_.uiBeginEdit (onId (h));
            setPlain (host_, onId (h), 1.0);
            host_.uiEndEdit (onId (h));
        }
        host_.uiBeginEdit (freqId (h));
        if (h < 3) host_.uiBeginEdit (gainId (h));
        startPos_ = e.position;
        startFreq_ = host_.parameters().get (freqId (h));
        startGain_ = h < 3 ? host_.parameters().get (gainId (h)) : 0.0;
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag_ < 0) return;
        const int h = drag_;
        const float k = e.mods.isShiftDown() ? 0.1f : 1.0f;   // Maj : déplacement fin
        const auto plot = plotArea();
        const double octaves = std::log2 (1000.0) * static_cast<double> ((e.position.x - startPos_.x) * k / plot.getWidth());
        const double lo = h == 4 ? 1000.0 : 20.0, hi = h == 3 ? 1000.0 : 20000.0;
        setPlain (host_, freqId (h), std::clamp (startFreq_ * std::pow (2.0, octaves), lo, hi));
        if (h < 3)
        {
            const double dDb = -static_cast<double> ((e.position.y - startPos_.y) * k / (plot.getHeight() * 0.5f)) * 18.0;
            setPlain (host_, gainId (h), std::clamp (startGain_ + dDb, -18.0, 18.0));
        }
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag_ < 0) return;
        if (drag_ < 3) host_.uiEndEdit (gainId (drag_));
        host_.uiEndEdit (freqId (drag_));
        drag_ = -1;
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h < 0) return;
        host_.uiBeginEdit (onId (h));
        setPlain (host_, onId (h), host_.parameters().get (onId (h)) >= 0.5 ? 0.0 : 1.0);
        host_.uiEndEdit (onId (h));
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int h = hit (e.position, 22.0f);
        if (h < 0 || w.deltaY == 0.0f) return;
        const int dir = w.deltaY > 0.0f ? 1 : -1;
        if (h < 3)
        {
            const auto id = ovd::bandParam (h, ovd::kQ);
            nova::setParam (host_, id, std::clamp (host_.parameters().get (id) * std::pow (1.12, dir), 0.1, 10.0));
        }
        else
        {
            const auto id = h == 3 ? ovd::eqHpSlope : ovd::eqLpSlope;
            nova::setParam (host_, id, std::clamp (static_cast<int> (std::lround (host_.parameters().get (id))) + dir, 0, 2));
        }
    }

private:
    static constexpr int kHandles = 5;
    static ParamId onId (int i) { return i < 3 ? ovd::bandParam (i, ovd::kOn) : i == 3 ? ovd::eqHpOn : ovd::eqLpOn; }
    static ParamId freqId (int i) { return i < 3 ? ovd::bandParam (i, ovd::kFreq) : i == 3 ? ovd::eqHp : ovd::eqLp; }
    static ParamId gainId (int i) { return ovd::bandParam (i, ovd::kGain); }
    static Token colourFor (int i) { return i == 0 ? Token::Band1 : i == 1 ? Token::Band2 : i == 2 ? Token::Band3 : Token::Pink; }
    juce::String text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }

    juce::String describe (int i) const
    {
        const double f = host_.parameters().get (freqId (i));
        const juce::String freq = f >= 1000.0 ? juce::String (f / 1000.0, 2) + " kHz" : juce::String (f, 0) + " Hz";
        if (i < 3)
        {
            static const char* const types[3] = { "cv.t_bell", "cv.t_low", "cv.t_high" };
            const int t = std::clamp (static_cast<int> (std::lround (host_.parameters().get (ovd::bandParam (i, ovd::kType)))), 0, 2);
            const double gain = host_.parameters().get (ovd::bandParam (i, ovd::kGain));
            return "EQ " + juce::String (i + 1) + "   " + text (types[t]) + "   " + freq + "   " + (gain >= 0.0 ? "+" : "") + juce::String (gain, 1) + " dB   Q " + juce::String (host_.parameters().get (ovd::bandParam (i, ovd::kQ)), 2);
        }
        const int slope = 12 << std::clamp (static_cast<int> (std::lround (host_.parameters().get (i == 3 ? ovd::eqHpSlope : ovd::eqLpSlope))), 0, 2);
        return text (i == 3 ? "cv.hp" : "cv.lp") + "   " + freq + "   " + juce::String (slope) + " dB/oct";
    }

    juce::Rectangle<float> plotArea() const
    {
        const auto b = getLocalBounds().toFloat();
        return { b.getX() + skin_.px (34.0f), b.getY() + skin_.px (10.0f), b.getWidth() - skin_.px (34.0f + 14.0f), b.getHeight() - skin_.px (10.0f + 22.0f) };
    }
    float xOf (double f) const { const auto p = plotArea(); return p.getX() + static_cast<float> (std::log (f / 20.0) / std::log (1000.0)) * p.getWidth(); }
    float yOf (double db) const { const auto p = plotArea(); return p.getCentreY() - static_cast<float> (db / 18.0) * p.getHeight() * 0.5f; }
    juce::Point<float> pointOf (int i) const
    {
        const double f = std::clamp (host_.parameters().get (freqId (i)), 20.0, 20000.0);
        const double g = i < 3 ? host_.parameters().get (gainId (i)) : 0.0;
        return { xOf (f), yOf (std::clamp (g, -18.0, 18.0)) };
    }
    int hit (juce::Point<float> p, float radius = 14.0f) const
    {
        int best = -1;
        float bestD = skin_.px (radius);
        for (int i = 0; i < kHandles; ++i)
        {
            const float d = pointOf (i).getDistanceFrom (p);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }
    void rebuild()
    {
        total_.clear();
        const double fs = std::max (8000.0, rate_());
        const auto plot = plotArea();
        const int n = std::max (2, static_cast<int> (plot.getWidth() / 3.0f));
        for (int i = 0; i <= n; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, static_cast<double> (i) / n);
            const double db = std::clamp (OverdriveModule::eqResponseDb (host_.parameters(), f, fs), -18.0, 18.0);
            if (i == 0) total_.startNewSubPath (xOf (f), yOf (db)); else total_.lineTo (xOf (f), yOf (db));
        }
        valid_ = true;
    }

    const nova::Skin& skin_;
    ParamHost& host_;
    const i18n::Translator& tr_;
    std::function<double()> rate_;
    juce::Path total_;
    bool valid_ = false;
    std::uint32_t key_ = 0xFFFFFFFFu;
    int drag_ = -1, hover_ = -1;
    juce::Point<float> startPos_;
    double startFreq_ = 1000.0, startGain_ = 0.0;
};

// ------------------------------------------------------------------------------------------------ éditeur
NovaFournaiseEditor::NovaFournaiseEditor (ParamHost& host, const OverdriveModule* module, const theme::Palette& palette, theme::KnobStyle knob,
                                          const i18n::Translator& tr, Options options)
    : EditorBase (host, palette, knob, tr, std::move (options), juce::String::fromUTF8 (std::string (tr.tr ("module.overdrive")).c_str()).toUpperCase()),
      module_ (module)
{
    addPage (tx ("fo.page_sat"), iconPath (0));
    addPage (tx ("fo.page_filters"), iconPath (1));
    addPage (tx ("fo.page_eq"), iconPath (2));
    addPage (tx ("fo.page_key"), iconPath (3));
    addPage (tx ("nv.page_settings"), iconPath (4));
    buildCommon();
    buildSaturationPage();
    buildFiltersPage();
    buildEqPage();
    buildKeyPage();
    buildSettingsPage (4);
    if (const auto names = presetNames(); ! names.empty()) presetPill()->setText (names[0]);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaFournaiseEditor::~NovaFournaiseEditor() = default;

std::vector<juce::String> NovaFournaiseEditor::presetNames() const
{
    std::vector<juce::String> out;
    for (const auto& p : ovd::factoryPresets()) out.push_back (juce::String::fromUTF8 (p.name));
    return out;
}

std::vector<std::pair<ParamId, double>> NovaFournaiseEditor::presetValues (int i) const { return ovd::factoryPresets()[static_cast<std::size_t> (i)].values; }

std::vector<ParamId> NovaFournaiseEditor::presetParamIds() const { return ovd::presetParams(); }

void NovaFournaiseEditor::buildCommon()
{
    metersPanel_ = add<nova::Panel> (-1, skin());
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();   // sous les entrées du menu, créées avant lui
    inMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.in"), nova::VMeter::Kind::Level, false);
    outMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.out"), nova::VMeter::Kind::Level, true);
    for (auto* m : { inMeter_, outMeter_ }) { m->setModeVisible (false); m->setZones (true); }
    resetPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    resetPill_->setText (tx ("fo.reset_peaks"));
    resetPill_->onClick = [this] { inMeter_->resetPeak(); outMeter_->resetPeak(); };
}

void NovaFournaiseEditor::buildSaturationPage()
{
    satPanel_ = add<nova::Panel> (0, skin());
    satPanel_->setTitle (tx ("fo.page_sat"));
    std::vector<juce::String> models;
    for (int i = 0; i < ovd::kNumModels; ++i) models.push_back (tx (("fo.m" + std::to_string (i)).c_str()));
    modelChips_ = add<ChipRow> (0, skin(), host(), ovd::model, models);
    transfer_ = add<TransferView> (0, skin(), host(), module_, translator());
    auto knob = [this] (ParamId id, const char* key, bool pink = false)
    {
        return add<nova::KnobCard> (0, skin(), host(), id, tx (key), modalOpener(), pink);
    };
    driveKnob_ = knob (ovd::drive, "fo.drive");
    biasKnob_ = knob (ovd::bias, "fo.bias");
    charKnob_ = knob (ovd::character, "fo.char");
    feelKnob_ = knob (ovd::feel, "fo.feel_tube");
    toneKnob_ = knob (ovd::tone, "fo.tone");
    levelKnob_ = knob (ovd::level, "fo.level", true);
    trimInKnob_ = knob (stock::inputTrim, "fo.input");
    mixKnob_ = knob (stock::mix, "fo.mix", true);
    trimOutKnob_ = knob (stock::outputTrim, "fo.output", true);
    smoothKnob_ = knob (ovd::smooth, "fo.smooth");
    tightKnob_ = knob (ovd::tight, "fo.tight");
    imperfectionKnob_ = knob (ovd::imperfection, "fo.imperfection");
    autoPill_ = add<nova::Pill> (0, skin());
    autoPill_->bindToggle (host(), ovd::autoGain, tx ("fo.autogain"));
    osPill_ = add<nova::Pill> (0, skin());
    osPill_->bindChoice (host(), ovd::oversampling, tx ("fo.os"), { "Off", "2x", "4x" });
    osNote_ = add<nova::Text> (0, skin(), Token::Pink, 11.0f);   // dans un insert de Neptune : la latence du suréchantillonnage n'est pas compensée
    chPill_ = add<nova::Pill> (0, skin());
    chPill_->bindChoice (host(), ovd::channels, tx ("fo.channels"), { tx ("fo.ch0"), tx ("fo.ch1"), tx ("fo.ch2"), tx ("fo.ch3") });
}

void NovaFournaiseEditor::retitleKnobs()
{
    const int m = std::clamp (choiceValue (ovd::model), 0, ovd::kNumModels - 1);
    if (m == shownModel_) return;
    shownModel_ = m;
    static const char* const feelKeys[ovd::kNumModels] = { "fo.feel_tube", "fo.feel_tape", "fo.feel_tr", "fo.feel_fuzz" };
    static const char* const biasKeys[ovd::kNumModels] = { "fo.bias", "fo.bias_unused", "fo.asym", "fo.bias" };
    feelKnob_->setTitle (tx (feelKeys[m]));
    biasKnob_->setTitle (tx (biasKeys[m]));
}

void NovaFournaiseEditor::buildFiltersPage()
{
    inFiltersPanel_ = add<nova::Panel> (1, skin());
    inFiltersPanel_->setTitle (tx ("fo.in_filters"));
    outFiltersPanel_ = add<nova::Panel> (1, skin());
    outFiltersPanel_->setTitle (tx ("fo.out_filters"));
    const std::vector<juce::String> slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    struct Def { ParamId on, freq, slope; const char* onKey; const char* freqKey; };
    const Def defs[4] = { { ovd::inHpOn, ovd::inHp, ovd::inHpSlope, "fo.hp", "fo.hp_freq" }, { ovd::inLpOn, ovd::inLp, ovd::inLpSlope, "fo.lp", "fo.lp_freq" },
                          { ovd::outHpOn, ovd::outHp, ovd::outHpSlope, "fo.hp", "fo.hp_freq" }, { ovd::outLpOn, ovd::outLp, ovd::outLpSlope, "fo.lp", "fo.lp_freq" } };
    for (std::size_t i = 0; i < 4; ++i)
    {
        auto& b = filters_[i];
        b.on = add<nova::Pill> (1, skin());
        b.on->bindToggle (host(), defs[i].on, tx (defs[i].onKey));
        b.freq = add<nova::KnobCard> (1, skin(), host(), defs[i].freq, tx (defs[i].freqKey), modalOpener(), i % 2 == 1);
        b.slope = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
        b.slope->bindChoice (host(), defs[i].slope, {}, slopes);
        b.slope->setFontSize (kPillFont);
    }
}

void NovaFournaiseEditor::buildEqPage()
{
    eqPanel_ = add<nova::Panel> (2, skin());
    eqPanel_->setTitle (tx ("cv.eq_title"));
    eqPill_ = add<nova::Pill> (2, skin());
    eqPill_->bindToggle (host(), ovd::eqOn, tx ("cv.eq_on"));
    const std::vector<juce::String> types { tx ("cv.t_bell"), tx ("cv.t_low"), tx ("cv.t_high") }, slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    eqView_ = add<EqView> (2, skin(), host(), translator(), [this] { const double r = options().sampleRate ? options().sampleRate() : 48000.0; return r > 0.0 ? r : 48000.0; });
    for (int b = 0; b < 3; ++b)
    {
        EqColumn c {};
        c.on = add<nova::Pill> (2, skin(), nova::Pill::Look::Small);
        c.on->bindToggle (host(), ovd::bandParam (b, ovd::kOn), "EQ " + juce::String (b + 1));
        c.type = add<nova::Pill> (2, skin(), nova::Pill::Look::Small);
        c.type->bindChoice (host(), ovd::bandParam (b, ovd::kType), {}, types);
        for (auto* pill : { c.on, c.type }) pill->setFontSize (kPillFont);
        eqColumns_.push_back (c);
    }
    for (int p = 0; p < 2; ++p)
    {
        EqColumn c {};
        c.on = add<nova::Pill> (2, skin(), nova::Pill::Look::Small);
        c.on->bindToggle (host(), p == 0 ? ovd::eqHpOn : ovd::eqLpOn, p == 0 ? tx ("fo.hp") : tx ("fo.lp"));
        c.slope = add<nova::Pill> (2, skin(), nova::Pill::Look::Small);
        c.slope->bindChoice (host(), p == 0 ? ovd::eqHpSlope : ovd::eqLpSlope, {}, slopes);
        for (auto* pill : { c.on, c.slope }) pill->setFontSize (kPillFont);
        eqColumns_.push_back (c);
    }
}

void NovaFournaiseEditor::buildKeyPage()
{
    keyPanel_ = add<nova::Panel> (3, skin());
    keyPanel_->setTitle (tx ("fo.page_key"));
    keySourcePill_ = add<nova::Pill> (3, skin());
    // dans un insert de Neptune il n'y a pas d'entrée latérale : seulement Aucune et Entrée principale
    if (options().externalKey) keySourcePill_->bindChoice (host(), ovd::keySource, tx ("fo.key_source"), { tx ("fo.ks0"), tx ("fo.ks1"), tx ("fo.ks2") });
    else keySourcePill_->bindChoice (host(), ovd::keySource, tx ("fo.key_source"), { tx ("fo.ks0"), tx ("fo.ks1") });
    for (const auto& k : { std::pair<ParamId, const char*> { ovd::keyAmount, "fo.key_amount" }, std::pair<ParamId, const char*> { ovd::keyAttack, "fo.key_attack" },
                           std::pair<ParamId, const char*> { ovd::keyRelease, "fo.key_release" }, std::pair<ParamId, const char*> { ovd::keyHp, "fo.key_hp" } })
        keyKnobs_.push_back (add<nova::KnobCard> (3, skin(), host(), k.first, tx (k.second), modalOpener(), false));
    keyView_ = add<KeyView> (3, skin(), host(), module_, translator());
    keyHint_ = add<nova::Text> (3, skin(), Token::TextDim2, 11.0f);
    keyHint_->setText (tx ("fo.hint_key"));
}

void NovaFournaiseEditor::layout()
{
    // ---- colonne de gauche (toutes les pages)
    metersPanel_->setBounds (R (14, 72, 176, 456));
    inMeter_->setBounds (R (24, 84, 74, 392));
    outMeter_->setBounds (R (106, 84, 74, 392));
    resetPill_->setBounds (R (24, 490, 152, 26));
    navPanel_->setBounds (R (14, 542, 176, 168));
    layoutNav (542, 168);

    // ---- page SATURATION
    satPanel_->setBounds (R (204, 72, 942, 638));
    modelChips_->setBounds (R (220, 108, 910, 40));
    transfer_->setBounds (R (220, 160, 420, 312));
    {
        // deux rangées de trois potards à droite de la courbe, puis la rangée entrée / mélange / sortie
        const float cw = 150.0f, gap = 12.0f, x0 = 656.0f;
        nova::KnobCard* top[3] = { driveKnob_, biasKnob_, charKnob_ };
        nova::KnobCard* mid[3] = { feelKnob_, toneKnob_, levelKnob_ };
        for (int i = 0; i < 3; ++i)
        {
            top[i]->setBounds (R (x0 + static_cast<float> (i) * (cw + gap), 160, cw, 150));
            mid[i]->setBounds (R (x0 + static_cast<float> (i) * (cw + gap), 322, cw, 150));
        }
        // rangée du bas : six potards de 140 sur la largeur de la page
        nova::KnobCard* low[6] = { trimInKnob_, mixKnob_, trimOutKnob_, smoothKnob_, tightKnob_, imperfectionKnob_ };
        for (int i = 0; i < 6; ++i) low[i]->setBounds (R (220.0f + static_cast<float> (i) * 154.0f, 536, 140, 150));
    }
    autoPill_->setBounds (R (220, 486, 200, 36));
    osPill_->setBounds (R (428, 486, 230, 36));
    osNote_->setBounds (R (220, 522, 910, 14));
    chPill_->setBounds (R (666, 486, 300, 36));

    // ---- page FILTRES : entrée (haut), sortie (bas) ; passe-haut puis passe-bas
    inFiltersPanel_->setBounds (R (204, 72, 942, 300));
    outFiltersPanel_->setBounds (R (204, 386, 942, 324));
    for (std::size_t i = 0; i < 4; ++i)
    {
        const float panelY = i < 2 ? 72.0f : 386.0f, x = 220.0f + static_cast<float> (i % 2) * 232.0f;
        const float y0 = panelY + 36.0f;
        filters_[i].on->setBounds (R (x, y0, 220, 32));
        filters_[i].freq->setBounds (R (x, y0 + 42.0f, 220, 140));
        filters_[i].slope->setBounds (R (x, y0 + 190.0f, 220, 28));
    }

    // ---- page EGALISEUR
    eqPanel_->setBounds (R (204, 72, 942, 638));
    eqPill_->setBounds (R (220, 108, 150, 30));
    eqView_->setBounds (R (220, 146, 910, 420));
    {
        const float cw = (910.0f - 12.0f * 4.0f) / 5.0f;
        for (std::size_t i = 0; i < eqColumns_.size(); ++i)
        {
            const auto& c = eqColumns_[i];
            const float x = 220.0f + static_cast<float> (i) * (cw + 12.0f);
            c.on->setBounds (R (x, 584, cw, 28));
            if (c.type != nullptr) c.type->setBounds (R (x, 620, cw, 28));
            if (c.slope != nullptr) c.slope->setBounds (R (x, 620, cw, 28));
        }
    }

    // ---- page ENTREE LATERALE
    keyPanel_->setBounds (R (204, 72, 942, 638));
    keySourcePill_->setBounds (R (220, 108, 300, 36));
    {
        const float cw = (910.0f - 12.0f * 3.0f) / 4.0f;
        for (std::size_t i = 0; i < keyKnobs_.size(); ++i) keyKnobs_[i]->setBounds (R (220.0f + static_cast<float> (i) * (cw + 12.0f), 160, cw, 160));
    }
    keyView_->setBounds (R (220, 340, 910, 90));
    keyHint_->setBounds (R (220, 446, 910, 80));

    // ---- en-tête et barre du bas
    if (abPill() != nullptr) abPill()->setBounds (R (14, 736, 40, 32));
    if (presetPill() != nullptr) presetPill()->setBounds (R (470, 11, 220, 36));
    if (bypassPill() != nullptr) bypassPill()->setBounds (R (1160 - 14 - 36, 11, 36, 36));
}

void NovaFournaiseEditor::tick()
{
    inMeter_->update (dbOf (module_->meter (ovd::kInPeakL)), dbOf (module_->meter (ovd::kInPeakR)));
    outMeter_->update (dbOf (module_->meter (ovd::kOutPeakL)), dbOf (module_->meter (ovd::kOutPeakR)));
    switch (currentPage())
    {
        case 0:
        {
            retitleKnobs(); transfer_->update();
            const int os = choiceValue (ovd::oversampling);
            const bool warn = ! options().brand && os > 0;
            osNote_->setVisible (warn);
            if (warn) osNote_->setText (juce::String::fromUTF8 (translator().format ("fo.os_note", { std::to_string (Oversampler::latencyFor (os >= 2 ? 4 : 2)) }).c_str()));
            break;
        }
        case 2: eqView_->update(); break;
        case 3: keyView_->update(); break;
        default: break;
    }
}
