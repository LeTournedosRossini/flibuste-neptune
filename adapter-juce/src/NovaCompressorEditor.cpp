#include "NovaCompressorEditor.h"

#include <algorithm>
#include <cmath>

#include "neptune/core/CompressorPresets.h"
#include "neptune/core/Oversampler.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace cm = neptune::core::compressor;

namespace
{
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
} // namespace

// ------------------------------------------------------------------------------------------------ réponse des filtres de la clé
class NovaCompressorEditor::SideEqView : public juce::Component
{
public:
    SideEqView (const nova::Skin& s, ParamHost& h, std::function<double()> rate) : skin_ (s), host_ (h), rate_ (std::move (rate)) {}

    void update()
    {
        std::uint32_t key = 0;
        for (int b = 0; b < cm::kBands; ++b)
            for (int k = 0; k < 6; ++k) key = key * 31u + host_.parameters().version (cm::bandParam (b, k));
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
        for (int d = -24; d <= 24; d += 12)
        {
            g.setColour (skin_.colour (Token::Grid, d == 0 ? 0.9f : 0.5f));
            g.drawHorizontalLine (juce::roundToInt (yOf (d)), plot.getX(), plot.getRight());
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (juce::String (d), juce::Rectangle<float> (b.getX() + skin_.px (2.0f), yOf (d) - skin_.px (6.0f), plot.getX() - b.getX() - skin_.px (6.0f), skin_.px (12.0f)), juce::Justification::centredRight);
        }
        if (! valid_) rebuild();
        if (anyOn_)
        {
            juce::Path filled (total_);
            filled.lineTo (plot.getRight(), yOf (0.0));
            filled.lineTo (plot.getX(), yOf (0.0));
            filled.closeSubPath();
            g.setColour (skin_.colour (Token::Accent, 0.12f));
            g.fillPath (filled);
            g.setColour (skin_.colour (Token::Accent));
            g.strokePath (total_, juce::PathStrokeType (skin_.px (2.2f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        // poignées des filtres en service
        for (int i = 0; i < cm::kBands; ++i)
        {
            if (host_.parameters().get (cm::bandParam (i, cm::kStatus)) < 1.5) continue;
            const auto pt = pointOf (i);
            const float r = skin_.px (i == drag_ ? 6.5f : 5.0f);
            g.setColour (skin_.colour (colourFor (i)));
            g.fillEllipse (pt.x - r, pt.y - r, r * 2, r * 2);
            g.setColour (skin_.colour (Token::StageBg));
            g.drawEllipse (pt.x - r, pt.y - r, r * 2, r * 2, skin_.px (1.2f));
        }
    }

    // ---- réglage à la souris : glisser une poignée (fréquence, et gain pour les types qui en ont), molette = Q (ou pente d'un passe-haut / passe-bas)
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (hit (e.position, 14.0f) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int b = hit (e.position, 14.0f);
        if (b < 0 || e.mods.isPopupMenu()) return;
        drag_ = b;
        host_.uiBeginEdit (cm::bandParam (b, cm::kFreq));
        if (hasGain (typeOf (b))) host_.uiBeginEdit (cm::bandParam (b, cm::kGain));
        startPos_ = e.position;
        startFreq_ = host_.parameters().get (cm::bandParam (b, cm::kFreq));
        startGain_ = host_.parameters().get (cm::bandParam (b, cm::kGain));
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag_ < 0) return;
        const int b = drag_;
        const float k = e.mods.isShiftDown() ? 0.1f : 1.0f;   // Maj : déplacement fin
        const auto plot = plotArea();
        const double octaves = std::log2 (1000.0) * static_cast<double> ((e.position.x - startPos_.x) * k / plot.getWidth());
        if (const auto* spec = host_.parameters().find (cm::bandParam (b, cm::kFreq)))
            host_.uiSetNormalised (cm::bandParam (b, cm::kFreq), spec->toNormalized (std::clamp (startFreq_ * std::pow (2.0, octaves), 20.0, 20000.0)));
        if (hasGain (typeOf (b)))
            if (const auto* spec = host_.parameters().find (cm::bandParam (b, cm::kGain)))
            {
                const double dDb = -static_cast<double> ((e.position.y - startPos_.y) * k / (plot.getHeight() * 0.5f)) * 24.0;
                host_.uiSetNormalised (cm::bandParam (b, cm::kGain), spec->toNormalized (std::clamp (startGain_ + dDb, -30.0, 30.0)));
            }
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag_ < 0) return;
        const int b = drag_;
        if (hasGain (typeOf (b))) host_.uiEndEdit (cm::bandParam (b, cm::kGain));
        host_.uiEndEdit (cm::bandParam (b, cm::kFreq));
        drag_ = -1;
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int b = hit (e.position, 22.0f);
        if (b < 0 || w.deltaY == 0.0f) return;
        const int dir = w.deltaY > 0.0f ? 1 : -1, t = typeOf (b);
        if (t == cm::kLowPass || t == cm::kHighPass)
        {
            const auto id = cm::bandParam (b, cm::kOrder);
            nova::setParam (host_, id, std::clamp (static_cast<int> (std::lround (host_.parameters().get (id))) + dir, 0, 5));
        }
        else
        {
            const auto id = cm::bandParam (b, cm::kQ);
            nova::setParam (host_, id, std::clamp (host_.parameters().get (id) * std::pow (1.12, dir), 0.025, 25.0));
        }
    }

private:
    static bool hasGain (int t) { return t == cm::kPeak || t == cm::kLowShelf || t == cm::kHighShelf || t == cm::kTiltShelf; }
    int typeOf (int b) const { return static_cast<int> (std::lround (host_.parameters().get (cm::bandParam (b, cm::kType)))); }
    // position d'une poignée : fréquence du filtre ; gain réglé pour les types qui en ont, sinon la ligne 0 dB
    juce::Point<float> pointOf (int b) const
    {
        const double f = std::clamp (host_.parameters().get (cm::bandParam (b, cm::kFreq)), 20.0, 20000.0);
        const double g = hasGain (typeOf (b)) ? host_.parameters().get (cm::bandParam (b, cm::kGain)) : 0.0;
        return { xOf (f), yOf (std::clamp (g, -24.0, 24.0)) };
    }
    int hit (juce::Point<float> p, float radius) const
    {
        int best = -1;
        float bestD = skin_.px (radius);
        for (int i = 0; i < cm::kBands; ++i)
        {
            if (host_.parameters().get (cm::bandParam (i, cm::kStatus)) < 1.5) continue;
            const float d = pointOf (i).getDistanceFrom (p);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }
    int drag_ = -1;
    juce::Point<float> startPos_;
    double startFreq_ = 1000.0, startGain_ = 0.0;

    static Token colourFor (int band) { return band == 1 ? Token::Band1 : band == 2 ? Token::Band2 : band == 3 ? Token::Band3 : Token::Accent; }
    juce::Rectangle<float> plotArea() const
    {
        const auto b = getLocalBounds().toFloat();
        return { b.getX() + skin_.px (34.0f), b.getY() + skin_.px (10.0f), b.getWidth() - skin_.px (34.0f + 14.0f), b.getHeight() - skin_.px (10.0f + 22.0f) };
    }
    float xOf (double f) const { const auto p = plotArea(); return p.getX() + static_cast<float> (std::log (f / 20.0) / std::log (1000.0)) * p.getWidth(); }
    float yOf (double db) const { const auto p = plotArea(); return p.getCentreY() - static_cast<float> (db / 24.0) * p.getHeight() * 0.5f; }
    void rebuild()
    {
        total_.clear();
        anyOn_ = false;
        for (int i = 0; i < cm::kBands; ++i) anyOn_ = anyOn_ || host_.parameters().get (cm::bandParam (i, cm::kStatus)) > 1.5;
        const double fs = rate_();
        const auto plot = plotArea();
        const int n = juce::jmax (2, static_cast<int> (plot.getWidth() / 3.0f));
        for (int i = 0; i <= n; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, static_cast<double> (i) / n);
            const double db = juce::jlimit (-24.0, 24.0, CompressorModule::sideResponseDb (host_.parameters(), f, fs));
            if (i == 0) total_.startNewSubPath (xOf (f), yOf (db)); else total_.lineTo (xOf (f), yOf (db));
        }
        valid_ = true;
    }

    const nova::Skin& skin_;
    ParamHost& host_;
    std::function<double()> rate_;
    juce::Path total_;
    bool valid_ = false, anyOn_ = false;
    std::uint32_t key_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ forme de la correspondance vélocité -> niveau (source MIDI)
class NovaCompressorEditor::MidiShapeView : public juce::Component
{
public:
    MidiShapeView (const nova::Skin& s, ParamHost& h, const CompressorModule* m, juce::String title) : skin_ (s), host_ (h), module_ (m), title_ (std::move (title)) {}

    void update()
    {
        const auto v = host_.parameters().version (cm::midiShape);
        const bool held = module_ != nullptr && module_->midiKey().noteHeld();
        const float level = module_ != nullptr ? module_->midiKey().shownLevel() : 0.0f;
        if (v == seen_ && held == held_ && std::fabs (level - level_) < 0.002f) return;
        seen_ = v;
        held_ = held;
        level_ = level;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (12.0f, true));
        g.drawText (title_, b.withHeight (skin_.px (15.0f)), juce::Justification::centred, true);
        const auto box = b.withTop (skin_.px (17.0f));
        skin_.fill (g, Token::StageBg, box, skin_.px (5.0f));
        const double shape = host_.parameters().get (cm::midiShape);
        auto at = [&] (double t, double y) { return juce::Point<float> (box.getX() + skin_.px (6.0f) + static_cast<float> (t) * (box.getWidth() - skin_.px (12.0f)),
                                                                          box.getBottom() - skin_.px (4.0f) - static_cast<float> (y) * (box.getHeight() - skin_.px (8.0f))); };
        g.setColour (skin_.colour (Token::Grid));
        g.drawLine ({ at (0.0, 0.0), at (1.0, 1.0) }, skin_.px (1.0f));   // droite de référence
        juce::Path curve;
        for (int i = 0; i <= 32; ++i)
        {
            const double t = i / 32.0;
            const auto pt = at (t, std::pow (t, exponent (shape)));
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.setColour (skin_.colour (Token::Accent));
        g.strokePath (curve, juce::PathStrokeType (skin_.px (2.0f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (held_ && level_ > 0.0f)
        {
            const auto pt = at (std::pow (static_cast<double> (level_), 1.0 / exponent (shape)), level_);
            g.setColour (skin_.colour (Token::Pink));
            g.fillEllipse (pt.x - skin_.px (4.0f), pt.y - skin_.px (4.0f), skin_.px (8.0f), skin_.px (8.0f));
        }
        g.setColour (skin_.colour (held_ ? Token::Pink : Token::Grid));   // voyant de note tenue
        g.fillEllipse (box.getX() + skin_.px (7.0f), box.getY() + skin_.px (8.0f), skin_.px (8.0f), skin_.px (8.0f));
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (11.0f));
        const juce::String text = std::fabs (shape) < 0.5 ? juce::String ("LINEAR") : (shape > 0 ? "EXP " : "LOG ") + juce::String (std::lround (std::fabs (shape))) + " %";
        g.drawText (text, box.reduced (skin_.px (6.0f), skin_.px (4.0f)), juce::Justification::topRight, true);
    }

    // glisser vers le bas : courbe plus convexe (EXP) ; vers le haut : LOG ; double-clic : linéaire
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        host_.uiBeginEdit (cm::midiShape);
        start_ = host_.parameters().get (cm::midiShape);
        dragging_ = true;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging_) return;
        const double plain = std::clamp (start_ + (e.position.y - e.mouseDownPosition.y) / skin_.px (1.0f) * (e.mods.isShiftDown() ? 0.2 : 1.0) * (200.0 / 120.0), -100.0, 100.0);
        if (const auto* spec = host_.parameters().find (cm::midiShape)) host_.uiSetNormalised (cm::midiShape, spec->toNormalized (plain));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging_) return;
        dragging_ = false;
        host_.uiEndEdit (cm::midiShape);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { host_.uiResetToDefault (cm::midiShape); repaint(); }

private:
    static double exponent (double shapePercent)
    {
        const double s = std::clamp (shapePercent, -100.0, 100.0) * 0.01;
        return s >= 0.0 ? 1.0 + 3.0 * s : 1.0 / (1.0 - 3.0 * s);
    }
    const nova::Skin& skin_;
    ParamHost& host_;
    const CompressorModule* module_;
    juce::String title_;
    std::uint32_t seen_ = 0xFFFFFFFFu;
    bool held_ = false, dragging_ = false;
    float level_ = 0.0f;
    double start_ = 0.0;
};

// ------------------------------------------------------------------------------------------------ zone de filtres de la clé
struct NovaCompressorEditor::Zone
{
    int band = 0;                                   // indice du filtre de la clé (0..7)
    int fixedType = cm::kPeak;                      // type imposé quand la zone est activée
    nova::Panel* card = nullptr;
    nova::Switch* sw = nullptr;
    std::vector<nova::KnobCard*> knobs;
    nova::Pill* slope = nullptr;
};

// ------------------------------------------------------------------------------------------------ éditeur
NovaCompressorEditor::NovaCompressorEditor (ParamHost& host, const CompressorModule* module, const theme::Palette& palette, theme::KnobStyle knob,
                                            const i18n::Translator& tr, Options options)
    : EditorBase (host, palette, knob, tr, std::move (options), juce::String::fromUTF8 (std::string (tr.tr ("module.compressor")).c_str()).toUpperCase()),
      module_ (module)
{
    addPage (tx ("nv.page_compressor"), iconPath (0));
    addPage (tx ("nv.page_sidechain"), iconPath (1));
    addPage (tx ("nv.page_advanced"), iconPath (2));
    addPage (tx ("nv.page_settings"), iconPath (3));
    buildCompressorPage();
    buildSidechainPage();
    buildAdvancedPage();
    buildBottomBar();
    buildSettingsPage (3);
    if (const auto names = presetNames(); ! names.empty()) presetPill()->setText (names[1]);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaCompressorEditor::~NovaCompressorEditor() = default;

std::vector<juce::String> NovaCompressorEditor::presetNames() const
{
    std::vector<juce::String> out;
    for (const auto& p : cm::factoryPresets()) out.push_back (juce::String::fromUTF8 (p.name));
    return out;
}

std::vector<std::pair<ParamId, double>> NovaCompressorEditor::presetValues (int i) const
{
    return cm::factoryPresets()[static_cast<std::size_t> (i)].values;
}

std::vector<ParamId> NovaCompressorEditor::presetParamIds() const { return cm::presetParams(); }

void NovaCompressorEditor::buildCompressorPage()
{
    metersPanel_ = add<nova::Panel> (-1, skin());
    addInfo (-1, metersPanel_, "hlp.cp_meters", true);
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();   // sous les entrées du menu, créées avant lui
    curvePanel_ = add<nova::Panel> (0, skin());
    curvePanel_->setTitle (tx ("nv.curve_title"));
    history_ = add<nova::CompressorHistoryView> (0, skin(), host(), module_);
    analyzerPill_ = add<nova::Pill> (0, skin(), nova::Pill::Look::Icon);
    {
        juce::Path gear;   // curseurs de réglage
        for (const float y : { 6.0f, 12.0f, 18.0f }) { gear.startNewSubPath (3.0f, y); gear.lineTo (21.0f, y); }
        gear.addEllipse (6.0f, 4.0f, 4.0f, 4.0f);
        gear.addEllipse (14.0f, 10.0f, 4.0f, 4.0f);
        gear.addEllipse (8.0f, 16.0f, 4.0f, 4.0f);
        analyzerPill_->setIcon (gear);
    }
    analyzerPill_->onClick = [this]
    {
        nova::CompressorHistoryView::showSettings (analyzerPill_, skin(), host(), [this] (const char* key) { return tx (key); });
    };
    inMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.in"), nova::VMeter::Kind::Level, false);
    grMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.gr"), nova::VMeter::Kind::Reduction, false);
    outMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.out"), nova::VMeter::Kind::Level, true);
    for (auto* m : { inMeter_, grMeter_, outMeter_ }) m->setModeVisible (false);
    // gains d'entrée et de sortie (réglages communs des modules) : pastilles à glisser sous les vu-mètres
    inTrimPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    inTrimPill_->bindValue (host(), stock::inputTrim, {});
    inTrimPill_->onClick = [this] { openModal (stock::inputTrim, uistate::Terminal::Pointer); };
    outTrimPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    outTrimPill_->bindValue (host(), stock::outputTrim, {});
    outTrimPill_->onClick = [this] { openModal (stock::outputTrim, uistate::Terminal::Pointer); };
    grMeter_->setDecayDbPerSecond (16.0f);   // retombée de la réduction : 16 dB/s, comme ZL
    curvePanel_->setMetaRightInset (36.0f);

    struct Spec { ParamId id; const char* title; bool pink; ParamId sub; const char* subTitle; };
    const Spec specs[6] = { { cm::threshold, "nv.threshold", false, cm::kneeWidth, "nv.knee" }, { cm::ratio, "nv.ratio", false, cm::curve, "nv.curve" },
                            { cm::attack, "nv.attack", false, cm::pump, "nv.pump" }, { cm::release, "nv.release", false, cm::smooth, "nv.smooth" },
                            { cm::makeup, "nv.makeup", true, cm::clipperDrive, "nv.clip" }, { cm::wet, "nv.mix", true, 0, nullptr } };
    for (const auto& s : specs)
    {
        Column c {};
        c.card = add<nova::Panel> (0, skin());
        c.knob = add<nova::KnobCard> (0, skin(), host(), s.id, tx (s.title), modalOpener(), s.pink);
        c.knob->setPlain (true);
        if (const auto* spec = host().parameters().find (s.id))
            c.knob->setRangeLabels (juce::String::fromUTF8 (host().parameters().toText (*spec, spec->min).c_str()),
                                    juce::String::fromUTF8 (host().parameters().toText (*spec, spec->max).c_str()));
        // course du potard réduite : attaque 0..50 ms, relâchement 40..350 ms, gain -12..+12 dB ; la modale (appui long) garde toute la plage du paramètre
        if (s.id == cm::attack) c.knob->setDisplayRange (0.0, 50.0, 2.0);
        else if (s.id == cm::release) c.knob->setDisplayRange (40.0, 350.0, 1.5);
        else if (s.id == cm::makeup) c.knob->setDisplayRange (-12.0, 12.0, 1.0);
        if (s.sub != 0)
        {
            c.sub = add<nova::SubSlider> (0, skin(), host(), s.sub, tx (s.subTitle), s.pink);
            const char* key = s.sub == cm::kneeWidth ? "hlp.knee" : s.sub == cm::curve ? "hlp.curve" : s.sub == cm::pump ? "hlp.pump" : s.sub == cm::smooth ? "hlp.smooth" : "hlp.clip";
            addInfo (0, c.sub, key, 2);   // sous le curseur, pas dessus
        }
        const char* mainHelp = s.id == cm::threshold ? "hlp.cp_threshold" : s.id == cm::ratio ? "hlp.cp_ratio" : s.id == cm::attack ? "hlp.cp_attack"
                             : s.id == cm::release ? "hlp.cp_release" : s.id == cm::makeup ? "hlp.cp_makeup" : "hlp.mix";
        addInfo (0, c.card, mainHelp, true);
        columns_.push_back (c);
    }
}

void NovaCompressorEditor::buildSidechainPage()
{
    scPanel_ = add<nova::Panel> (1, skin());
    scPanel_->setTitle (tx ("nv.sc_title"));
    sourcePill_ = add<nova::Pill> (1, skin());
    if (options().midiSource)
        sourcePill_->bindChoice (host(), cm::externalSide, tx ("nv.source"), { tx ("nv.src_internal"), tx ("nv.src_external"), tx ("nv.src_midi") });
    else
        sourcePill_->bindChoice (host(), cm::externalSide, tx ("nv.source"), { tx ("nv.src_internal"), tx ("nv.src_external") });
    listenPill_ = add<nova::Pill> (1, skin());
    listenPill_->bindToggle (host(), cm::sideOut, tx ("nv.listen"));
    eqPill_ = add<nova::Pill> (1, skin());
    eqPill_->bindChoice (host(), cm::sideEqBypass, {}, { tx ("nv.eq_on"), tx ("nv.eq_off") });
    // allumer l'égaliseur de la clé sans aucune zone en service pose un jeu de départ : passe-haut et deux cloches
    eqPill_->onClick = [this]
    {
        if (value (cm::sideEqBypass) >= 0.5) return;   // éteint
        for (const auto& z : zones_)
            if (choiceValue (cm::bandParam (z->band, cm::kStatus)) == cm::kOn) return;
        const double freqs[3] = { 100.0, 250.0, 3000.0 };
        for (int i = 0; i < 3 && i < static_cast<int> (zones_.size()); ++i)
        {
            const auto& z = zones_[static_cast<std::size_t> (i)];
            nova::setParam (host(), cm::bandParam (z->band, cm::kType), z->fixedType);
            nova::setParam (host(), cm::bandParam (z->band, cm::kFreq), freqs[i]);
            nova::setParam (host(), cm::bandParam (z->band, cm::kStatus), cm::kOn);
        }
    };
    rmsPill_ = add<nova::Pill> (1, skin());
    rmsPill_->bindToggle (host(), cm::rmsOn, tx ("nv.rms_on"));
    keyGain_ = add<nova::SubSlider> (1, skin(), host(), cm::sideGain, tx ("nv.side_gain"));
    rmsLength_ = add<nova::SubSlider> (1, skin(), host(), cm::rmsLength, tx ("nv.rms_length"));
    shapeView_ = add<MidiShapeView> (1, skin(), host(), module_, tx ("nv.midi_shape"));
    channelPill_ = add<nova::Pill> (1, skin());
    channelPill_->onClick = [this]
    {
        juce::PopupMenu menu;
        for (int c = 0; c <= 16; ++c) menu.addItem (c + 1, c == 0 ? tx ("nv.midi_all") : juce::String (c), true, choiceValue (cm::midiChannel) == c);
        juce::Component::SafePointer<juce::Component> safe (this);   // (MSVC : pas de `this` dans la capture d'initialisation d'une lambda imbriquée)
        menu.setLookAndFeel (&channelPill_->getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (channelPill_), [this, safe] (int r)
        {
            if (safe != nullptr && r > 0) nova::setParam (host(), cm::midiChannel, r - 1);
        });
    };
    midiNote_ = add<nova::Text> (1, skin(), Token::TextDim, 12.0f, true);
    midiNote_->setText (tx ("nv.midi_note"));
    addInfo (1, channelPill_, "hlp.cp_midichannel");
    addInfo (1, sourcePill_, "hlp.source");
    addInfo (1, listenPill_, "hlp.listen");
    addInfo (1, eqPill_, "hlp.eqswitch");
    addInfo (1, rmsPill_, "hlp.rmsswitch");
    addInfo (1, keyGain_, "hlp.keygain");
    addInfo (1, rmsLength_, "hlp.rmslength");
    sideEq_ = add<SideEqView> (1, skin(), host(), [this] { const double r = options().sampleRate ? options().sampleRate() : 48000.0; return r > 0.0 ? r : 48000.0; });

    // cinq zones : passe-haut (filtre 1), trois cloches (filtres 2 à 4), passe-bas (filtre 5) ; les trois autres filtres restent accessibles à l'hôte
    struct ZSpec { int band; int type; const char* title; int knobs; };   // knobs : 0 freq seul (+ pente), 1 gain / freq / Q
    const ZSpec zs[5] = { { 0, cm::kHighPass, "nv.zone_hp", 0 }, { 1, cm::kPeak, "nv.zone_band", 1 }, { 2, cm::kPeak, "nv.zone_band", 1 },
                          { 3, cm::kPeak, "nv.zone_band", 1 }, { 4, cm::kLowPass, "nv.zone_lp", 0 } };
    for (const auto& z : zs)
    {
        auto zone = std::make_unique<Zone>();
        zone->band = z.band;
        zone->fixedType = z.type;
        zone->card = add<nova::Panel> (1, skin());
        zone->card->setTitle (tx (z.title) + (z.knobs == 1 ? " " + juce::String (z.band) : juce::String()));
        zone->sw = add<nova::Switch> (1, skin());
        const int band = z.band, type = z.type;
        zone->sw->onChange = [this, band, type] (bool on)
        {
            if (on)
            {
                if (choiceValue (cm::bandParam (band, cm::kType)) != type) nova::setParam (host(), cm::bandParam (band, cm::kType), type);
                nova::setParam (host(), cm::bandParam (band, cm::kStatus), cm::kOn);
            }
            else nova::setParam (host(), cm::bandParam (band, cm::kStatus), cm::kOff);
        };
        auto knobFor = [&] (int k, const char* title)
        {
            auto* kc = add<nova::KnobCard> (1, skin(), host(), cm::bandParam (z.band, k), tx (title), modalOpener(), false);
            kc->setPlain (true);
            kc->setCompact (true);
            zone->knobs.push_back (kc);
        };
        if (z.knobs == 1) { knobFor (cm::kGain, "nv.gain"); knobFor (cm::kFreq, "nv.freq"); knobFor (cm::kQ, "nv.q"); }
        else
        {
            knobFor (cm::kFreq, "nv.freq");
            zone->slope = add<nova::Pill> (1, skin(), nova::Pill::Look::Small);
            zone->slope->bindChoice (host(), cm::bandParam (z.band, cm::kOrder), tx ("nv.slope"),
                                     { tx ("nv.slope_6"), tx ("nv.slope_12"), tx ("nv.slope_24"), tx ("nv.slope_48"), tx ("nv.slope_72"), tx ("nv.slope_96") });
        }
        addInfo (1, zone->knobs[0], z.type == cm::kHighPass ? "hlp.cp_zone_hp" : z.type == cm::kLowPass ? "hlp.cp_zone_lp" : "hlp.cp_zone_band", true);
        zones_.push_back (std::move (zone));
    }
}

void NovaCompressorEditor::buildAdvancedPage()
{
    advPanel_ = add<nova::Panel> (2, skin());
    advPanel_->setTitle (tx ("nv.adv_title"));
    struct P { ParamId id; const char* key; };
    for (const auto& p : { P { cm::stereoSwap, "nv.swap" }, P { cm::rangeInf, "nv.range_inf" }, P { cm::rmsOn, "nv.rms_on" }, P { cm::delta, "nv.delta" } })
    {
        auto* pill = add<nova::Pill> (2, skin());
        pill->bindToggle (host(), p.id, tx (p.key));
        advPills_.push_back (pill);
        addInfo (2, pill, p.id == cm::stereoSwap ? "hlp.swap" : p.id == cm::rangeInf ? "hlp.rangeinf" : p.id == cm::rmsOn ? "hlp.rmsswitch" : "hlp.delta");
    }
    for (const auto& p : { P { cm::floorDb, "nv.floor" }, P { cm::range, "nv.range" }, P { cm::hold, "nv.hold" }, P { cm::stereoLink, "nv.link" }, P { cm::wet1, "nv.wet_a" },
                           P { cm::wet2, "nv.wet_b" }, P { cm::rmsLength, "nv.rms_length" }, P { cm::rmsSpeed, "nv.rms_speed" }, P { cm::rmsMix, "nv.rms_mix" } })
    {
        auto* kc = add<nova::KnobCard> (2, skin(), host(), p.id, tx (p.key), modalOpener(), false);
        advCards_.push_back (kc);
        const char* hk = p.id == cm::floorDb ? "hlp.floor" : p.id == cm::range ? "hlp.range" : p.id == cm::hold ? "hlp.hold" : p.id == cm::stereoLink ? "hlp.link"
                       : p.id == cm::wet1 || p.id == cm::wet2 ? "hlp.wet" : p.id == cm::rmsLength ? "hlp.rmslength" : p.id == cm::rmsSpeed ? "hlp.rmsspeed" : "hlp.rmsmix";
        addInfo (2, kc, hk, true);
    }
}

void NovaCompressorEditor::buildBottomBar()
{
    stereoPill_ = add<nova::Pill> (-1, skin());
    stereoPill_->bindChoice (host(), cm::stereoMode, tx ("nv.stereo_mode"), { tx ("nv.stereo_ms"), tx ("nv.stereo_lr"), tx ("nv.stereo_msmax"), tx ("nv.stereo_lrmax") });
    stylePill_ = add<nova::Pill> (-1, skin());
    stylePill_->bindChoice (host(), cm::style, tx ("nv.style"), { tx ("nv.style_clean"), tx ("nv.style_classic"), tx ("nv.style_optical"), tx ("nv.style_vocal") });
    dirPill_ = add<nova::Pill> (-1, skin());
    dirPill_->bindChoice (host(), cm::direction, tx ("nv.direction"), { tx ("nv.dir_compress"), tx ("nv.dir_inflate"), tx ("nv.dir_expand"), tx ("nv.dir_shape") });
    deltaPill_ = add<nova::Pill> (-1, skin());
    deltaPill_->bindToggle (host(), cm::delta, tx ("nv.delta"));
    osPill_ = add<nova::Pill> (-1, skin());
    osPill_->bindChoice (host(), cm::oversample, tx ("nv.oversampling"), { tx ("nv.os_off"), tx ("nv.os_2x"), tx ("nv.os_4x") });
    lookPill_ = add<nova::Pill> (-1, skin());
    lookPill_->bindValue (host(), cm::lookahead, tx ("nv.lookahead"));   // glisser pour régler ; double-clic : saisie
    lookPill_->onClick = [this] { openModal (cm::lookahead, uistate::Terminal::Pointer); };
    addInfo (-1, stereoPill_, "hlp.stereo");
    addInfo (-1, stylePill_, "hlp.style");
    addInfo (-1, dirPill_, "hlp.mode");
    addInfo (-1, deltaPill_, "hlp.delta");
    addInfo (-1, osPill_, "hlp.oversampling");
    addInfo (-1, lookPill_, "hlp.lookahead");
}

void NovaCompressorEditor::layout()
{
    // ---- page COMPRESSOR
    metersPanel_->setBounds (R (14, 72, 176, 410));
    {
        const auto in = metersPanel_->getBounds().reduced (skin().ipx (8.0f), skin().ipx (12.0f));
        const int w = in.getWidth() / 3, trimH = skin().ipx (40.0f);
        const auto meters = in.withTrimmedBottom (trimH);
        inMeter_->setBounds (meters.withWidth (w));
        grMeter_->setBounds (meters.withX (meters.getX() + w).withWidth (w));
        outMeter_->setBounds (meters.withX (meters.getX() + 2 * w).withWidth (w));
        const int py = in.getBottom() - skin().ipx (30.0f), ph = skin().ipx (24.0f);
        inTrimPill_->setBounds (in.getX() + skin().ipx (1.0f), py, w - skin().ipx (2.0f), ph);
        outTrimPill_->setBounds (in.getX() + 2 * w + skin().ipx (1.0f), py, w - skin().ipx (2.0f), ph);
    }
    navPanel_->setBounds (R (14, 496, 176, 214));
    layoutNav (496, 214);
    curvePanel_->setBounds (R (204, 72, 942, 410));
    history_->setBounds (R (220, 100, 910, 366));
    analyzerPill_->setBounds (R (1146 - 14 - 28, 76, 28, 24));
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
    sourcePill_->setBounds (R (220, 108, 150, 30));
    listenPill_->setBounds (R (398, 108, 90, 30));
    eqPill_->setBounds (R (516, 108, 84, 30));
    rmsPill_->setBounds (R (628, 108, 60, 30));
    keyGain_->setBounds (R (724, 100, 176, 48));
    rmsLength_->setBounds (R (930, 100, 176, 48));
    shapeView_->setBounds (R (1018, 74, 112, 122));   // petit et presque carré, son titre à la hauteur de celui du panneau
    channelPill_->setBounds (R (398, 108, 176, 30));
    sideEq_->setBounds (R (220, 152, 910, 314));
    midiNote_->setBounds (R (220, 250, 780, 60));
    const float unit = (942.0f - 12.0f * 4.0f) / 13.0f;
    float zx = 204.0f;
    for (auto& z : zones_)
    {
        const float zw = unit * static_cast<float> (z->knobs.size() == 3 ? 3 : 2);
        z->card->setBounds (R (zx, 496, zw, 214));
        z->sw->setBounds (R (zx + zw - 8 - 34, 496 + 10, 34, 19));
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
            z->knobs[0]->setBounds (R (zx + 6, 496 + 40, inner, 84));
            z->slope->setBounds (R (zx + 14, 496 + 150, zw - 28, 26));
        }
        zx += zw + 12.0f;
    }

    // ---- page ADVANCED
    advPanel_->setBounds (R (204, 72, 942, 638));
    for (std::size_t i = 0; i < advPills_.size(); ++i) advPills_[i]->setBounds (R (220 + 150.0f * static_cast<float> (i), 108, 120, 30));
    const float cw = (910.0f - 12.0f * 4.0f) / 5.0f;
    for (std::size_t i = 0; i < advCards_.size(); ++i)
    {
        const float x = 220.0f + static_cast<float> (i % 5) * (cw + 12.0f), y = 160.0f + static_cast<float> (i / 5) * 160.0f;
        advCards_[i]->setBounds (R (x, y, cw, 144));
    }

    // ---- barre du bas
    const float by = 724.0f + 12.0f;
    abPill()->setBounds (R (14, 724 + 12, 40, 32));
    float x = 58.0f;
    for (auto* p : { stereoPill_, stylePill_, dirPill_, deltaPill_, osPill_, lookPill_ })
    {
        const float w = p == stylePill_ ? 124.0f : p == dirPill_ ? 134.0f : p == deltaPill_ ? 64.0f : p == osPill_ ? 154.0f : p == lookPill_ ? 150.0f : 112.0f;
        p->setBounds (R (x, by, w, 32));
        x += w + 26.0f;   // place du bouton « ? »
    }

    // ---- en-tête
    presetPill()->setBounds (R (470, 11, 220, 36));
    if (bypassPill() != nullptr) bypassPill()->setBounds (R (1160 - 14 - 36, 11, 36, 36));
}

void NovaCompressorEditor::tick()
{
    // vu-mètres, courbe, légende
    float inDb = -120.0f, grDb = 0.0f;
    if (module_ != nullptr)
    {
        const float il = dbOf (module_->meter (cm::kInPeakL)), ir = dbOf (module_->meter (cm::kInPeakR));
        const float ol = dbOf (module_->meter (cm::kOutPeakL)), orr = dbOf (module_->meter (cm::kOutPeakR));
        const float gl = std::max (0.0f, -module_->meter (cm::kReductionL)), gr = std::max (0.0f, -module_->meter (cm::kReductionR));
        inMeter_->update (il, ir);
        outMeter_->update (ol, orr);
        grMeter_->update (gl, gr);
        inDb = std::max (il, ir);
        grDb = -std::max (gl, gr);
    }
    const auto meta = tx ("nv.in") + " " + (inDb < -100.0f ? juce::String ("-inf") : juce::String (inDb, 1)) + " dB   " + tx ("nv.gr") + " " + juce::String (-grDb, 1) + " dB";
    if (meta != metaText_) { metaText_ = meta; curvePanel_->setMeta (meta); }
    sideEq_->update();

    // zones de la clé : interrupteur = filtre en service ; potards estompés quand la zone est inactive
    for (auto& z : zones_)
    {
        const bool on = choiceValue (cm::bandParam (z->band, cm::kStatus)) == cm::kOn;
        z->sw->setOn (on);
        for (auto* k : z->knobs) k->setAlpha (on ? 1.0f : 0.4f);
        if (z->slope != nullptr) z->slope->setAlpha (on ? 1.0f : 0.4f);
    }
    const int source = choiceValue (cm::externalSide);
    const bool midi = source == cm::kSourceMidi && options().midiSource;
    sourcePill_->setTintToken (source != cm::kSourceInternal ? Token::Pink : Token::Accent);
    // source MIDI : la clé est un niveau constant dérivé de la vélocité, sans filtres, ni gain, ni détecteur RMS, ni écoute
    if (currentPage() == 1)
    {
        keyGain_->setVisible (! midi);
        rmsLength_->setVisible (! midi);
        rmsPill_->setVisible (! midi);
        shapeView_->setVisible (midi);
        channelPill_->setVisible (midi);
        midiNote_->setVisible (midi);
        eqPill_->setVisible (! midi);
        listenPill_->setVisible (! midi);
        sideEq_->setVisible (! midi);
        for (auto* c : { static_cast<juce::Component*> (keyGain_), static_cast<juce::Component*> (rmsLength_), static_cast<juce::Component*> (rmsPill_),
                         static_cast<juce::Component*> (eqPill_), static_cast<juce::Component*> (listenPill_) })
            setInfoVisible (c, ! midi);   // leurs boutons « ? » disparaissent avec eux
    }
    shapeView_->update();
    channelPill_->setText (tx ("nv.midi_channel") + "  " + (choiceValue (cm::midiChannel) == 0 ? tx ("nv.midi_all") : juce::String (choiceValue (cm::midiChannel))));

    // anticipation et latence
    const double la = value (cm::lookahead);
    const double rate = options().sampleRate ? options().sampleRate() : 48000.0;
    const int os = choiceValue (cm::oversample);
    const int latency = (la > 0.0 ? static_cast<int> (std::lround (la * 0.001 * rate)) : 0) + Oversampler::latencyFor (os >= 2 ? 4 : os == 1 ? 2 : 1);
    latencyWarn_ = ! options().brand && latency > 0;   // dans un insert de Neptune, la latence n'est pas compensée entre les pistes
    const auto lat = juce::String::fromUTF8 (translator().format (latencyWarn_ ? "nv.latency_insert" : "nv.latency", { std::to_string (latency) }).c_str());
    if (lat != latencyText_) { latencyText_ = lat; repaint (R (920, 716, 226, 64)); }
}

void NovaCompressorEditor::paintBackground (juce::Graphics& g)
{
    g.setColour (skin().colour (latencyWarn_ ? Token::Pink : Token::TextDim2));
    g.setFont (skin().font (11.0f, true));
    if (latencyWarn_) g.drawFittedText (latencyText_, R (930, 724 + 4, 216, 44).getSmallestIntegerContainer(), juce::Justification::centredRight, 3);
    else g.drawText (latencyText_, R (930, 724 + 12, 216, 32).toFloat(), juce::Justification::centredRight);
}
