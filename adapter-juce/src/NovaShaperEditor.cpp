#include "NovaShaperEditor.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "neptune/core/Oversampler.h"
#include "neptune/core/ShaperPresets.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace sh = neptune::core::shaper;

namespace
{
constexpr float kPi = 3.14159265358979f;
float dbOf (float lin) { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }

juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0)   // barres
    {
        for (const float x : { 4.0f, 12.0f, 20.0f })
        {
            p.startNewSubPath (x, 20.0f);
            p.lineTo (x, x == 4.0f ? 10.0f : x == 12.0f ? 4.0f : 13.0f);
        }
    }
    else if (kind == 1)   // chemin de la clé
    {
        p.startNewSubPath (6.0f, 3.0f);
        p.lineTo (6.0f, 9.0f);
        p.quadraticTo (6.0f, 12.0f, 9.0f, 12.0f);
        p.lineTo (15.0f, 12.0f);
        p.quadraticTo (18.0f, 12.0f, 18.0f, 15.0f);
        p.lineTo (18.0f, 21.0f);
        p.addEllipse (4.4f, 1.4f, 3.2f, 3.2f);
        p.addEllipse (16.4f, 19.4f, 3.2f, 3.2f);
    }
    else if (kind == 2)   // avancé
    {
        p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
        p.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
    }
    else   // réglages : curseurs
    {
        for (const float y : { 6.0f, 12.0f, 18.0f }) { p.startNewSubPath (3.0f, y); p.lineTo (21.0f, y); }
        p.addEllipse (6.0f, 4.0f, 4.0f, 4.0f);
        p.addEllipse (14.0f, 10.0f, 4.0f, 4.0f);
        p.addEllipse (8.0f, 16.0f, 4.0f, 4.0f);
    }
    return p;
}

// FFT radix 2 en place (taille puissance de 2), pour l'analyseur de la clé
void fft (std::vector<std::complex<float>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * kPi / static_cast<float> (len);
        const std::complex<float> wl (std::cos (ang), std::sin (ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (std::size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void setPlain (ParamHost& host, ParamId id, double v)
{
    if (const auto* spec = host.parameters().find (id)) host.uiSetNormalised (id, spec->toNormalized (v));
}
} // namespace

// ------------------------------------------------------------------------------------------------ vue RESPONSE : poids du niveau (LEVEL, THRESHOLD, KNEE)
class NovaShaperEditor::ResponseView : public juce::Component
{
public:
    ResponseView (const nova::Skin& s, ParamHost& h, juce::String hint) : skin_ (s), host_ (h), hint_ (std::move (hint)) {}

    void update()
    {
        std::uint32_t key = 0;
        for (const auto id : { sh::levelDependency, sh::levelThreshold, sh::levelKnee }) key = key * 31u + host_.parameters().version (id);
        if (key == key_) return;
        key_ = key;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        skin_.fill (g, Token::StageBg, b, skin_.px (6.0f));
        const auto plot = plotArea();
        const double level = host_.parameters().get (sh::levelDependency) / 100.0, thr = host_.parameters().get (sh::levelThreshold), knee = host_.parameters().get (sh::levelKnee);
        g.setColour (skin_.colour (Token::Grid, 0.6f));
        for (int d = -60; d <= 0; d += 12) g.drawVerticalLine (static_cast<int> (xOf (d)), plot.getY(), plot.getBottom());
        for (int i = 0; i <= 4; ++i) g.drawHorizontalLine (static_cast<int> (plot.getY() + plot.getHeight() * static_cast<float> (i) / 4.0f), plot.getX(), plot.getRight());
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (9.0f));
        for (int d = -60; d <= 0; d += 12)
            g.drawText (juce::String (d), juce::Rectangle<float> (xOf (d) - skin_.px (16.0f), plot.getBottom() + skin_.px (3.0f), skin_.px (32.0f), skin_.px (12.0f)), juce::Justification::centred);
        g.setFont (skin_.font (11.0f));
        g.setColour (skin_.colour (Token::TextDim));
        g.drawText (hint_, juce::Rectangle<float> (plot.getX() + skin_.px (6.0f), plot.getY() + skin_.px (4.0f), plot.getWidth() - skin_.px (12.0f), skin_.px (16.0f)), juce::Justification::topLeft, true);
        juce::Path p;
        const double lv = std::fabs (level);
        for (int i = 0; i <= 240; ++i)
        {
            const double db = -60.0 + 60.0 * i / 240.0;
            double c = knee < 0.01 ? (db > thr ? 1.0 : 0.0) : std::clamp (0.5 + (db - thr) / knee, 0.0, 1.0);
            c = c * c * (3.0 - 2.0 * c);
            const double w = level >= 0.0 ? (1.0 - lv) + lv * c : (1.0 - lv) + lv * (1.0 - c);
            const float x = xOf (db), y = plot.getBottom() - static_cast<float> (w) * plot.getHeight();
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (skin_.colour (Token::Accent));
        g.strokePath (p, juce::PathStrokeType (skin_.px (2.2f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // poignées : seuil (cercle), coude (carrés)
        const float cy = plot.getY() + plot.getHeight() * 0.5f;
        const float r = skin_.px (6.0f);
        g.setColour (skin_.colour (Token::Hring));
        g.fillEllipse (xOf (thr) - r - 2.0f, cy - r - 2.0f, r * 2 + 4.0f, r * 2 + 4.0f);
        g.setColour (skin_.colour (Token::Handle));
        g.fillEllipse (xOf (thr) - r, cy - r, r * 2, r * 2);
        if (knee >= 0.01)
        {
            g.setColour (skin_.colour (Token::KnobHot));
            for (const double db : { thr - knee * 0.5, thr + knee * 0.5 })
                g.fillRect (xOf (db) - skin_.px (4.0f), cy + skin_.px (16.0f), skin_.px (8.0f), skin_.px (8.0f));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        editing_ = 0;
        if (e.mods.isPopupMenu()) return;
        const double thr = host_.parameters().get (sh::levelThreshold), knee = host_.parameters().get (sh::levelKnee);
        const auto plot = plotArea();
        const float cy = plot.getY() + plot.getHeight() * 0.5f;
        if (std::hypot (e.position.x - xOf (thr), e.position.y - cy) < skin_.px (14.0f)) editing_ = 1;
        else if (std::fabs (e.position.y - (cy + skin_.px (20.0f))) < skin_.px (12.0f)
                 && (std::fabs (e.position.x - xOf (thr - knee * 0.5)) < skin_.px (12.0f) || std::fabs (e.position.x - xOf (thr + knee * 0.5)) < skin_.px (12.0f))) editing_ = 2;
        if (editing_ != 0) host_.uiBeginEdit (editing_ == 1 ? sh::levelThreshold : sh::levelKnee);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (editing_ == 1) setPlain (host_, sh::levelThreshold, std::clamp (dbAt (e.position.x), -60.0, 0.0));
        else if (editing_ == 2) setPlain (host_, sh::levelKnee, std::clamp (2.0 * std::fabs (dbAt (e.position.x) - host_.parameters().get (sh::levelThreshold)), 0.0, 24.0));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (editing_ != 0) host_.uiEndEdit (editing_ == 1 ? sh::levelThreshold : sh::levelKnee);
        editing_ = 0;
    }

private:
    juce::Rectangle<float> plotArea() const
    {
        const auto b = getLocalBounds().toFloat();
        return { b.getX() + skin_.px (18.0f), b.getY() + skin_.px (10.0f), b.getWidth() - skin_.px (36.0f), b.getHeight() - skin_.px (30.0f) };
    }
    float xOf (double db) const { const auto p = plotArea(); return p.getX() + static_cast<float> ((db + 60.0) / 60.0) * p.getWidth(); }
    double dbAt (float x) const { const auto p = plotArea(); return (x - p.getX()) / p.getWidth() * 60.0 - 60.0; }

    const nova::Skin& skin_;
    ParamHost& host_;
    juce::String hint_;
    std::uint32_t key_ = 0xFFFFFFFFu;
    int editing_ = 0;
};

// ------------------------------------------------------------------------------------------------ égaliseur de détection (réponse, poignées, analyseur de la clé)
class NovaShaperEditor::EqView : public juce::Component
{
public:
    EqView (const nova::Skin& s, ParamHost& h, const ShaperModule* m, std::function<double()> rate)
        : skin_ (s), host_ (h), module_ (m), rate_ (std::move (rate)), key_ (4096), spectrum_ (64, -120.0f) {}

    void update()
    {
        // l'analyseur de la clé bouge en permanence tant qu'il est allumé ; sinon, seule une modification des réglages redessine
        std::uint32_t k = 0;
        for (ParamId id = sh::scEqOn; id <= sh::scBand3Q; ++id) k = k * 31u + host_.parameters().version (id);
        const bool spectrum = host_.parameters().get (sh::scAnalyzerOn) >= 0.5;
        if (spectrum || k != key_version_) { key_version_ = k; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        skin_.fill (g, Token::StageBg, r, skin_.px (6.0f));
        plot_ = r.reduced (skin_.px (36.0f), skin_.px (14.0f));
        const bool eqOn = host_.parameters().get (sh::scEqOn) >= 0.5;
        g.setColour (skin_.colour (Token::Grid, 0.6f));
        for (const double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 }) g.drawVerticalLine (static_cast<int> (xOf (f)), plot_.getY(), plot_.getBottom());
        for (const double d : { -12.0, -6.0, 0.0, 6.0, 12.0 }) g.drawHorizontalLine (static_cast<int> (yOf (d)), plot_.getX(), plot_.getRight());
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (9.0f));
        for (const double f : { 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0 })
            g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String (f, 0),
                        juce::Rectangle<float> (xOf (f) - skin_.px (16.0f), plot_.getBottom() + skin_.px (3.0f), skin_.px (32.0f), skin_.px (12.0f)), juce::Justification::centred);
        for (const double d : { -12.0, 0.0, 12.0 })
            g.drawText (juce::String (d, 0), juce::Rectangle<float> (r.getX() + skin_.px (2.0f), yOf (d) - skin_.px (6.0f), skin_.px (30.0f), skin_.px (12.0f)), juce::Justification::centredRight);
        paintSpectrum (g);
        juce::Path p;
        for (int i = 0; i <= 300; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 300.0);
            const double db = eqOn && module_ != nullptr ? module_->detectionResponseDb (host_.parameters(), f) : 0.0;
            const float x = xOf (f), y = yOf (std::clamp (db, -24.0, 18.0));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (skin_.colour (Token::Accent, eqOn ? 1.0f : 0.35f));
        g.strokePath (p, juce::PathStrokeType (skin_.px (2.2f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // poignées : HP et LP sur la ligne de 0 dB, bandes à (f, gain)
        for (int h = 0; h < 5; ++h)
        {
            const auto pt = handle (h);
            const float rr = skin_.px (h == active_ ? 7.0f : 6.0f);
            g.setColour (skin_.colour (h == 0 || h == 4 ? Token::Band1 : Token::Band2, eqOn ? 1.0f : 0.35f));
            g.fillEllipse (pt.x - rr, pt.y - rr, rr * 2, rr * 2);
            g.setColour (skin_.colour (Token::BgApp));
            g.drawEllipse (pt.x - rr, pt.y - rr, rr * 2, rr * 2, skin_.px (1.2f));
        }
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (hit (e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        active_ = e.mods.isPopupMenu() ? -1 : hit (e.position);
        if (active_ < 0) return;
        host_.uiBeginEdit (freqId (active_));
        if (isBand (active_)) host_.uiBeginEdit (freqId (active_) + 1);
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (active_ < 0) return;
        setPlain (host_, freqId (active_), std::clamp (fOf (e.position.x), 20.0, 20000.0));
        if (isBand (active_)) setPlain (host_, freqId (active_) + 1, std::clamp (dbAt (e.position.y), -18.0, 18.0));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (active_ >= 0)
        {
            host_.uiEndEdit (freqId (active_));
            if (isBand (active_)) host_.uiEndEdit (freqId (active_) + 1);
        }
        active_ = -1;
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h >= 1 && h <= 3) host_.uiResetToDefault (freqId (h) + 1);
    }
    // molette près d'une poignée : Q d'une bande, pente d'un passe-haut / passe-bas
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int h = hit (e.position, 24.0f);
        if (h < 0) return;
        const double step = w.deltaY > 0 ? 1.0 : -1.0;
        if (isBand (h))
        {
            const ParamId q = freqId (h) + 2;
            nova::setParam (host_, q, std::clamp (host_.parameters().get (q) * std::pow (1.15, step), 0.3, 12.0));
        }
        else
        {
            const ParamId slope = h == 0 ? sh::scHpSlope : sh::scLpSlope;
            nova::setParam (host_, slope, std::clamp (host_.parameters().get (slope) + 6.0 * step, 6.0, 48.0));
        }
        repaint();
    }

private:
    static bool isBand (int h) { return h >= 1 && h <= 3; }
    static ParamId freqId (int h) { return h == 0 ? sh::scHpFreq : h == 4 ? sh::scLpFreq : sh::scBand1Freq + 3 * static_cast<ParamId> (h - 1); }
    float xOf (double f) const { return plot_.getX() + static_cast<float> (std::log (f / 20.0) / std::log (1000.0)) * plot_.getWidth(); }
    double fOf (float x) const { return 20.0 * std::pow (1000.0, (x - plot_.getX()) / plot_.getWidth()); }
    float yOf (double db) const { return plot_.getY() + static_cast<float> ((18.0 - db) / 36.0) * plot_.getHeight(); }
    double dbAt (float y) const { return 18.0 - (y - plot_.getY()) / plot_.getHeight() * 36.0; }
    juce::Point<float> handle (int h) const
    {
        const double f = host_.parameters().get (freqId (h));
        const double gain = isBand (h) ? host_.parameters().get (freqId (h) + 1) : 0.0;
        return { xOf (f), yOf (gain) };
    }
    int hit (juce::Point<float> p, float radius = 14.0f) const
    {
        int best = -1;
        float bestD = skin_.px (radius);
        for (int h = 0; h < 5; ++h)
            if (const float d = handle (h).getDistanceFrom (p); d < bestD) { bestD = d; best = h; }
        return best;
    }

    void paintSpectrum (juce::Graphics& g)
    {
        if (module_ == nullptr || host_.parameters().get (sh::scAnalyzerOn) < 0.5) return;
        const int n = 2048;
        module_->copyKey (key_.data(), n);
        std::vector<std::complex<float>> a (static_cast<std::size_t> (n));
        for (int i = 0; i < n; ++i) a[static_cast<std::size_t> (i)] = key_[static_cast<std::size_t> (i)] * (0.5f - 0.5f * std::cos (2.0f * kPi * static_cast<float> (i) / static_cast<float> (n)));
        fft (a);
        const double fs = rate_ ? rate_() : 48000.0;
        const int bands = static_cast<int> (spectrum_.size());
        juce::Path p;
        for (int b = 0; b < bands; ++b)
        {
            const double f0 = 20.0 * std::pow (1000.0, static_cast<double> (b) / bands), f1 = 20.0 * std::pow (1000.0, static_cast<double> (b + 1) / bands);
            const int k0 = std::max (1, static_cast<int> (f0 / fs * n)), k1 = std::max (k0 + 1, std::min (n / 2, static_cast<int> (f1 / fs * n)));
            float m = 0.0f;
            for (int k = k0; k < k1; ++k) m = std::max (m, std::abs (a[static_cast<std::size_t> (k)]));
            const float db = dbOf (m / (static_cast<float> (n) * 0.25f));
            auto& s = spectrum_[static_cast<std::size_t> (b)];
            s = db > s ? db : s * 0.92f + db * 0.08f;
            const float x = xOf (std::sqrt (f0 * f1)), y = plot_.getBottom() - juce::jlimit (0.0f, 1.0f, (s + 90.0f) / 90.0f) * plot_.getHeight();
            if (b == 0) { p.startNewSubPath (x, plot_.getBottom()); p.lineTo (x, y); } else p.lineTo (x, y);
        }
        p.lineTo (xOf (20000.0), plot_.getBottom());
        p.closeSubPath();
        g.setColour (skin_.colour (Token::Accent2, 0.22f));
        g.fillPath (p);
    }

    const nova::Skin& skin_;
    ParamHost& host_;
    const ShaperModule* module_;
    std::function<double()> rate_;
    juce::Rectangle<float> plot_;
    std::vector<float> key_;
    std::vector<float> spectrum_;
    std::uint32_t key_version_ = 0xFFFFFFFFu;
    int active_ = -1;
};

// ------------------------------------------------------------------------------------------------ zone de réglages de la clé
struct NovaShaperEditor::Zone
{
    int kind = 0;                      // 0 passe-haut, 1 à 3 bandes, 4 passe-bas
    nova::Panel* card = nullptr;
    std::vector<nova::KnobCard*> knobs;
    nova::Pill* slope = nullptr;       // passe-haut et passe-bas : pente par clic
    ParamId slopeId = 0;
};

// ------------------------------------------------------------------------------------------------ éditeur
NovaShaperEditor::NovaShaperEditor (ParamHost& host, const ShaperModule* module, const theme::Palette& palette, theme::KnobStyle knob,
                                    const i18n::Translator& tr, Options options)
    : EditorBase (host, palette, knob, tr, std::move (options), juce::String::fromUTF8 (std::string (tr.tr ("module.shaper")).c_str()).toUpperCase()),
      module_ (module)
{
    addPage (tx ("shp.page_shaper"), iconPath (0));
    addPage (tx ("shp.page_sidechain"), iconPath (1));
    addPage (tx ("nv.page_advanced"), iconPath (2));
    addPage (tx ("shp.page_settings"), iconPath (3));
    buildShaperPage();
    buildSidechainPage();
    buildAdvancedPage();
    buildBottomBar();
    buildSettingsPage (3);
    if (const auto names = presetNames(); ! names.empty()) presetPill()->setText (names[0]);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaShaperEditor::~NovaShaperEditor() = default;

std::vector<juce::String> NovaShaperEditor::presetNames() const
{
    std::vector<juce::String> out;
    for (const auto& p : sh::factoryPresets()) out.push_back (juce::String::fromUTF8 (p.name));
    return out;
}

std::vector<std::pair<ParamId, double>> NovaShaperEditor::presetValues (int i) const { return sh::factoryPresets()[static_cast<std::size_t> (i)].values; }

std::vector<ParamId> NovaShaperEditor::presetParamIds() const { return sh::presetParams(); }

void NovaShaperEditor::buildShaperPage()
{
    metersPanel_ = add<nova::Panel> (-1, skin());
    addInfo (-1, metersPanel_, "hlp.sh_meters", true);
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();   // sous les entrées du menu, créées avant lui
    detPanel_ = add<nova::Panel> (0, skin());
    detPanel_->setTitle (tx ("nv.sh_title"));
    wave_ = add<nova::ShaperWaveView> (0, skin(), host(), module_);
    response_ = add<ResponseView> (0, skin(), host(), tx ("shp.response_hint"));
    viewPill_ = add<nova::Pill> (0, skin());
    viewPill_->onClick = [this] { showResponse_ = ! showResponse_; };
    gearPill_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Icon);
    gearPill_->setIcon (iconPath (3));
    gearPill_->onClick = [this] { nova::ShaperWaveView::showSettings (gearPill_, skin(), host(), [this] (const char* key) { return tx (key); }); };
    detPanel_->setMetaRightInset (36.0f + 120.0f);

    inMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.in"), nova::VMeter::Kind::Level, false);
    outMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.out"), nova::VMeter::Kind::Level, true);
    for (auto* m : { inMeter_, outMeter_ }) m->setModeVisible (false);
    inTrimPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    inTrimPill_->bindValue (host(), stock::inputTrim, {});
    inTrimPill_->onClick = [this] { openModal (stock::inputTrim, uistate::Terminal::Pointer); };

    // curseurs sous le graphe : temps de détection, lissage, plage, coude, liaison
    struct Bar { ParamId id; const char* key; };
    for (const Bar b : { Bar { sh::attackTime, "shp.attack_time" }, Bar { sh::sustainTime, "shp.sustain_time" }, Bar { sh::shapeSmoothing, "shp.smoothing" },
                         Bar { sh::range, "shp.range" }, Bar { sh::levelKnee, "shp.knee" }, Bar { sh::stereoLink, "shp.link" } })
    {
        auto* bar = add<nova::SubSlider> (0, skin(), host(), b.id, tx (b.key), false);
        bars_.push_back (bar);
        const char* hk = b.id == sh::attackTime ? "hlp.sh_attacktime" : b.id == sh::sustainTime ? "hlp.sh_sustaintime" : b.id == sh::shapeSmoothing ? "hlp.sh_smoothing"
                       : b.id == sh::range ? "hlp.sh_range" : b.id == sh::levelKnee ? "hlp.sh_knee" : "hlp.sh_link";
        addInfo (0, bar, hk, 2);   // sous le curseur
    }

    struct Spec { ParamId id; const char* title; bool pink; ParamId sub; const char* subTitle; };
    const Spec specs[6] = { { sh::attackGain1, "shp.attack", false, sh::attackGain2, "shp.attack_2" }, { sh::sustainGain1, "shp.sustain", false, sh::sustainGain2, "shp.sustain_2" },
                            { sh::levelDependency, "shp.level", false, 0, nullptr }, { sh::levelThreshold, "shp.threshold", false, 0, nullptr },
                            { stock::outputTrim, "shp.output", true, 0, nullptr }, { stock::mix, "shp.mix", true, 0, nullptr } };
    for (const auto& s : specs)
    {
        Column c {};
        c.card = add<nova::Panel> (0, skin());
        c.knob = add<nova::KnobCard> (0, skin(), host(), s.id, tx (s.title), modalOpener(), s.pink);
        c.knob->setPlain (true);
        if (const auto* spec = host().parameters().find (s.id))
            c.knob->setRangeLabels (juce::String::fromUTF8 (host().parameters().toText (*spec, spec->min).c_str()),
                                    juce::String::fromUTF8 (host().parameters().toText (*spec, spec->max).c_str()));
        const char* hk = s.id == sh::attackGain1 ? "hlp.sh_attack" : s.id == sh::sustainGain1 ? "hlp.sh_sustain" : s.id == sh::levelDependency ? "hlp.sh_level"
                       : s.id == sh::levelThreshold ? "hlp.sh_threshold" : s.id == stock::outputTrim ? "hlp.sh_output" : s.id == stock::mix ? "hlp.sh_mix" : nullptr;
        if (hk != nullptr) addInfo (0, c.card, hk, 1);
        if (s.sub != 0)
        {
            c.sub = add<nova::SubSlider> (0, skin(), host(), s.sub, tx (s.subTitle), s.pink);
            addInfo (0, c.sub, s.sub == sh::attackGain2 ? "hlp.sh_attack2" : "hlp.sh_sustain2", 2);
        }
        columns_.push_back (c);
    }
}

void NovaShaperEditor::buildSidechainPage()
{
    scPanel_ = add<nova::Panel> (1, skin());
    scPanel_->setTitle (tx ("nv.sc_title"));
    enablePill_ = add<nova::Pill> (1, skin());
    enablePill_->bindToggle (host(), sh::scEnable, tx ("shp.sc_enable"));
    sourcePill_ = add<nova::Pill> (1, skin());
    if (options().externalKey)
        sourcePill_->bindChoice (host(), sh::scSource, tx ("nv.source"), { tx ("shp.sc_internal"), tx ("shp.sc_external"), tx ("shp.sc_mix") });
    else
    {
        sourcePill_->setText (tx ("nv.source") + "  " + tx ("shp.sc_internal"));
        sourcePill_->setStatic (true);   // External et Mix n'existent pas dans un insert de Neptune
    }
    addInfo (1, sourcePill_, "hlp.sh_source");
    listenPill_ = add<nova::Pill> (1, skin());
    listenPill_->bindToggle (host(), sh::scListen, tx ("shp.sc_listen"));
    eqPill_ = add<nova::Pill> (1, skin());
    eqPill_->bindToggle (host(), sh::scEqOn, tx ("shp.sc_eq"));
    spectrumPill_ = add<nova::Pill> (1, skin());
    spectrumPill_->bindToggle (host(), sh::scAnalyzerOn, tx ("shp.sc_spectrum"));
    gainSlider_ = add<nova::SubSlider> (1, skin(), host(), sh::scGain, tx ("shp.sc_gain"));
    intVolSlider_ = add<nova::SubSlider> (1, skin(), host(), sh::scIntVol, tx ("shp.sc_int_vol"));
    extVolSlider_ = add<nova::SubSlider> (1, skin(), host(), sh::scExtVol, tx ("shp.sc_ext_vol"));
    phaseIntPill_ = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
    phaseIntPill_->bindToggle (host(), sh::scPhaseInt, tx ("shp.sc_phase_int"));
    phaseExtPill_ = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
    phaseExtPill_->bindToggle (host(), sh::scPhaseExt, tx ("shp.sc_phase_ext"));
    extMsPill_ = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
    extMsPill_->bindToggle (host(), sh::scExtMs, tx ("shp.sc_ext_ms"));
    addInfo (1, gainSlider_, "hlp.sh_scgain", 2);
    addInfo (1, intVolSlider_, "hlp.sh_mixvol", 2);
    addInfo (1, extVolSlider_, "hlp.sh_mixvol", 2);
    addInfo (1, spectrumPill_, "hlp.sh_spectrum");
    scNote_ = add<nova::Text> (1, skin(), Token::TextDim2, 11.0f);
    scNote_->setText (tx ("nv.sh_insert_note"));
    addInfo (1, listenPill_, "hlp.listen");
    addInfo (1, eqPill_, "hlp.eqswitch");
    eq_ = add<EqView> (1, skin(), host(), module_, [this] { const double r = options().sampleRate ? options().sampleRate() : 48000.0; return r > 0.0 ? r : 48000.0; });

    // cinq zones : passe-haut, trois bandes (cloches), passe-bas
    for (int k = 0; k < 5; ++k)
    {
        auto zone = std::make_unique<Zone>();
        zone->kind = k;
        zone->card = add<nova::Panel> (1, skin());
        zone->card->setTitle (k == 0 ? tx ("nv.zone_hp") : k == 4 ? tx ("nv.zone_lp") : tx ("nv.zone_band") + " " + juce::String (k));
        auto knobFor = [&] (ParamId id, const char* title)
        {
            auto* kc = add<nova::KnobCard> (1, skin(), host(), id, tx (title), modalOpener(), false);
            kc->setPlain (true);
            kc->setCompact (true);
            zone->knobs.push_back (kc);
        };
        if (k >= 1 && k <= 3)
        {
            const ParamId base = sh::scBand1Freq + 3 * static_cast<ParamId> (k - 1);
            knobFor (base + 1, "nv.gain");
            knobFor (base, "nv.freq");
            knobFor (base + 2, "nv.q");
        }
        else
        {
            knobFor (k == 0 ? sh::scHpFreq : sh::scLpFreq, "nv.freq");
            zone->slopeId = k == 0 ? sh::scHpSlope : sh::scLpSlope;
            zone->slope = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
            const ParamId sid = zone->slopeId;
            zone->slope->onClick = [this, sid]
            {
                const double next = value (sid) + 6.0 > 48.0 ? 6.0 : value (sid) + 6.0;   // 6, 12 ... 48 dB/oct
                nova::setParam (host(), sid, next);
            };
        }
        zones_.push_back (std::move (zone));
    }
}

void NovaShaperEditor::buildAdvancedPage()
{
    advPanel_ = add<nova::Panel> (2, skin());
    advPanel_->setTitle (tx ("nv.adv_title"));
    detPill_ = add<nova::Pill> (2, skin());
    detPill_->bindChoice (host(), sh::detMode, tx ("shp.set_detection"), { tx ("shp.det_peak"), tx ("shp.det_rms") });
    struct P { ParamId id; const char* key; };
    addInfo (2, detPill_, "hlp.sh_detect");
    for (const auto& p : { P { sh::detFastRatio, "shp.set_ratio" }, P { sh::noiseFloor, "shp.set_floor" }, P { sh::meterPeakDecay, "shp.set_peak_decay" } })
    {
        advCards_.push_back (add<nova::KnobCard> (2, skin(), host(), p.id, tx (p.key), modalOpener(), false));
        addInfo (2, advCards_.back(), p.id == sh::detFastRatio ? "hlp.sh_ratio" : p.id == sh::noiseFloor ? "hlp.sh_floor" : "hlp.sh_peakdecay", 1);
    }
}

void NovaShaperEditor::buildBottomBar()
{
    chPill_ = add<nova::Pill> (-1, skin());
    chPill_->bindChoice (host(), sh::channelMode, {}, { tx ("shp.ch_stereo"), tx ("shp.ch_dual"), tx ("shp.ch_ms") });
    deltaPill_ = add<nova::Pill> (-1, skin());
    deltaPill_->bindToggle (host(), sh::delta, tx ("shp.delta"));
    osPill_ = add<nova::Pill> (-1, skin());
    osPill_->bindChoice (host(), sh::oversampling, tx ("nv.oversampling"), { tx ("nv.os_off"), tx ("nv.os_2x"), tx ("nv.os_4x") });
    lookPill_ = add<nova::Pill> (-1, skin());
    lookPill_->bindValue (host(), sh::lookahead, tx ("nv.lookahead"));   // glisser pour régler ; double-clic : saisie
    lookPill_->onClick = [this] { openModal (sh::lookahead, uistate::Terminal::Pointer); };
    addInfo (-1, chPill_, "hlp.sh_channel");
    addInfo (-1, deltaPill_, "hlp.delta");
    addInfo (-1, osPill_, "hlp.oversampling");
    addInfo (-1, lookPill_, "hlp.lookahead");
}

void NovaShaperEditor::layout()
{
    // ---- page SHAPER
    metersPanel_->setBounds (R (14, 72, 176, 410));
    {
        const auto in = metersPanel_->getBounds().reduced (skin().ipx (8.0f), skin().ipx (12.0f));
        const int w = in.getWidth() / 2, trimH = skin().ipx (40.0f);
        const auto meters = in.withTrimmedBottom (trimH);
        inMeter_->setBounds (meters.withWidth (w));
        outMeter_->setBounds (meters.withX (meters.getX() + w).withWidth (w));
        inTrimPill_->setBounds (in.getX() + skin().ipx (1.0f), in.getBottom() - skin().ipx (30.0f), w - skin().ipx (2.0f), skin().ipx (24.0f));
    }
    navPanel_->setBounds (R (14, 496, 176, 214));
    layoutNav (496, 214);
    detPanel_->setBounds (R (204, 72, 942, 410));
    wave_->setBounds (R (220, 104, 910, 300));
    response_->setBounds (R (220, 104, 910, 300));
    gearPill_->setBounds (R (1146 - 14 - 28, 76, 28, 24));
    viewPill_->setBounds (R (1146 - 14 - 28 - 8 - 112, 76, 112, 24));
    const float bw = (910.0f - 10.0f * 5.0f) / 6.0f;
    for (std::size_t i = 0; i < bars_.size(); ++i) bars_[i]->setBounds (R (220.0f + static_cast<float> (i) * (bw + 10.0f), 420, bw, 52));
    const float colW = (942.0f - 12.0f * 5.0f) / 6.0f;
    for (std::size_t i = 0; i < columns_.size(); ++i)
    {
        const float x = 204.0f + static_cast<float> (i) * (colW + 12.0f);
        columns_[i].card->setBounds (R (x, 496, colW, 214));
        columns_[i].knob->setBounds (R (x, 496, colW, 132));
        if (columns_[i].sub != nullptr) columns_[i].sub->setBounds (R (x + 10, 496 + 134, colW - 20, 48));
    }

    // ---- page SIDECHAIN
    scPanel_->setBounds (R (204, 72, 942, 410));
    enablePill_->setBounds (R (220, 104, 110, 30));
    sourcePill_->setBounds (R (340, 104, 170, 30));
    listenPill_->setBounds (R (540, 104, 96, 30));
    eqPill_->setBounds (R (666, 104, 120, 30));
    spectrumPill_->setBounds (R (816, 104, 110, 30));
    gainSlider_->setBounds (R (220, 140, 220, 48));
    intVolSlider_->setBounds (R (220, 140, 220, 48));
    extVolSlider_->setBounds (R (460, 140, 220, 48));
    phaseIntPill_->setBounds (R (700, 148, 90, 28));
    phaseExtPill_->setBounds (R (800, 148, 90, 28));
    extMsPill_->setBounds (R (900, 148, 110, 28));
    scNote_->setBounds (R (460, 152, 660, 32));
    eq_->setBounds (R (220, 196, 910, 270));
    const float unit = (942.0f - 12.0f * 4.0f) / 13.0f;
    float zx = 204.0f;
    for (auto& z : zones_)
    {
        const float zw = unit * static_cast<float> (z->knobs.size() == 3 ? 3 : 2);
        z->card->setBounds (R (zx, 496, zw, 214));
        const float inner = zw - 12.0f;
        if (z->knobs.size() == 3)
        {
            // gain et fréquence sur la première ligne, Q sur la seconde (centré)
            const float kw = inner / 2.0f;
            z->knobs[0]->setBounds (R (zx + 6, 496 + 34, kw, 84));
            z->knobs[1]->setBounds (R (zx + 6 + kw, 496 + 34, kw, 84));
            z->knobs[2]->setBounds (R (zx + 6 + kw / 2.0f, 496 + 34 + 84, kw, 84));
        }
        else
        {
            z->knobs[0]->setBounds (R (zx + 6, 496 + 34, inner, 100));
            z->slope->setBounds (R (zx + 14, 496 + 150, zw - 28, 26));
        }
        zx += zw + 12.0f;
    }

    // ---- page ADVANCED
    advPanel_->setBounds (R (204, 72, 942, 638));
    detPill_->setBounds (R (220, 108, 200, 30));
    const float cw = (910.0f - 12.0f * 4.0f) / 5.0f;
    for (std::size_t i = 0; i < advCards_.size(); ++i) advCards_[i]->setBounds (R (220.0f + static_cast<float> (i) * (cw + 12.0f), 160, cw, 144));

    // ---- barre du bas
    const float by = 724.0f + 12.0f;
    abPill()->setBounds (R (14, 724 + 12, 40, 32));
    float x = 58.0f;
    for (auto* p : { chPill_, deltaPill_, osPill_, lookPill_ })
    {
        const float w = p == chPill_ ? 140.0f : p == deltaPill_ ? 64.0f : p == osPill_ ? 154.0f : 150.0f;
        p->setBounds (R (x, by, w, 32));
        x += w + 26.0f;   // place du bouton « ? »
    }

    // ---- en-tête
    presetPill()->setBounds (R (470, 11, 220, 36));
    if (bypassPill() != nullptr) bypassPill()->setBounds (R (1160 - 14 - 36, 11, 36, 36));
}

void NovaShaperEditor::tick()
{
    // vu-mètres et légende
    float gainDb = 0.0f;
    if (module_ != nullptr)
    {
        const float decay = static_cast<float> (value (sh::meterPeakDecay));
        inMeter_->setDecayDbPerSecond (decay);
        outMeter_->setDecayDbPerSecond (decay);
        inMeter_->update (dbOf (module_->meter (sh::kInPeakL)), dbOf (module_->meter (sh::kInPeakR)));
        outMeter_->update (dbOf (module_->meter (sh::kOutPeakL)), dbOf (module_->meter (sh::kOutPeakR)));
        gainDb = std::fabs (module_->meter (sh::kGainL)) > std::fabs (module_->meter (sh::kGainR)) ? module_->meter (sh::kGainL) : module_->meter (sh::kGainR);
    }
    const auto meta = tx ("nv.sh_gain") + " " + (gainDb >= 0.05f ? "+" : "") + juce::String (gainDb, 1) + " dB";
    if (meta != metaText_) { metaText_ = meta; detPanel_->setMeta (meta); }

    if (currentPage() == 0)
    {
        wave_->setVisible (! showResponse_);
        response_->setVisible (showResponse_);
        viewPill_->setText (showResponse_ ? tx ("nv.sh_response") : tx ("nv.sh_wave"));
        response_->update();
        const bool dual = choiceValue (sh::channelMode) != 0;   // jeu 2 : utile en Dual Mono et M-S seulement
        for (auto& c : columns_)
            if (c.sub != nullptr) c.sub->setAlpha (dual ? 1.0f : 0.4f);
    }
    if (currentPage() == 1)
    {
        eq_->update();
        const bool eqOn = value (sh::scEqOn) >= 0.5;
        const int source = options().externalKey ? choiceValue (sh::scSource) : 0;
        const bool mix = source == 2;
        gainSlider_->setVisible (! mix);
        intVolSlider_->setVisible (mix);
        extVolSlider_->setVisible (mix);
        phaseIntPill_->setVisible (mix);
        phaseExtPill_->setVisible (mix);
        extMsPill_->setVisible (source != 0);
        scNote_->setVisible (! options().externalKey);
        sourcePill_->setTintToken (source != 0 ? Token::Pink : Token::Accent);
        for (auto& z : zones_)
        {
            for (auto* k : z->knobs) k->setAlpha (eqOn ? 1.0f : 0.4f);
            if (z->slope != nullptr)
            {
                z->slope->setAlpha (eqOn ? 1.0f : 0.4f);
                z->slope->setText (tx ("nv.slope") + "  " + juce::String (juce::roundToInt (value (z->slopeId))) + " dB/oct");
            }
        }
    }

    // latence
    const double look = value (sh::lookahead);
    const double rate = options().sampleRate ? options().sampleRate() : 48000.0;
    const int os = choiceValue (sh::oversampling);
    const int latency = (look > 0.0 ? static_cast<int> (std::lround (look * 0.001 * rate)) : 0) + Oversampler::latencyFor (os >= 2 ? 4 : os == 1 ? 2 : 1);
    latencyWarn_ = ! options().brand && latency > 0;   // dans un insert de Neptune, la latence n'est pas compensée entre les pistes
    const auto lat = juce::String::fromUTF8 (translator().format (latencyWarn_ ? "nv.latency_insert" : "nv.latency", { std::to_string (latency) }).c_str());
    if (lat != latencyText_) { latencyText_ = lat; repaint (R (920, 716, 226, 64)); }
}

void NovaShaperEditor::paintBackground (juce::Graphics& g)
{
    g.setColour (skin().colour (latencyWarn_ ? Token::Pink : Token::TextDim2));
    g.setFont (skin().font (11.0f, true));
    if (latencyWarn_) g.drawFittedText (latencyText_, R (930, 724 + 4, 216, 44).getSmallestIntegerContainer(), juce::Justification::centredRight, 3);
    else g.drawText (latencyText_, R (930, 724 + 12, 216, 32).toFloat(), juce::Justification::centredRight);
}
