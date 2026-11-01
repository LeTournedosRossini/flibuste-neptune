#include "NovaDigueEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace neptune::core;
using neptune::core::theme::Token;
namespace lm = neptune::core::limiter;

namespace
{
float dbOf (float lin) { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }

juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0)   // plafond et signal écrêté
    {
        p.startNewSubPath (3.0f, 6.0f); p.lineTo (21.0f, 6.0f);
        p.startNewSubPath (3.0f, 18.0f); p.lineTo (8.0f, 18.0f); p.lineTo (10.0f, 6.0f); p.lineTo (14.0f, 6.0f); p.lineTo (16.0f, 18.0f); p.lineTo (21.0f, 18.0f);
    }
    else
    {
        p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
        p.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
    }
    return p;
}

void setPlain (ParamHost& host, ParamId id, double v)
{
    if (const auto* spec = host.parameters().find (id)) host.uiSetNormalised (id, spec->toNormalized (v));
}
} // namespace

// ------------------------------------------------------------------------------------------------ historique de la réduction de gain
class NovaDigueEditor::HistoryView : public juce::Component
{
public:
    explicit HistoryView (const nova::Skin& s) : skin_ (s) { history_.fill (0.0f); }

    void push (float reductionDb)
    {
        history_[head_] = std::max (0.0f, reductionDb);
        head_ = (head_ + 1) % history_.size();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        skin_.fill (g, Token::StageBg, getLocalBounds().toFloat(), skin_.px (6.0f));
        const auto plot = getLocalBounds().toFloat().reduced (skin_.px (14.0f), skin_.px (22.0f)).withTrimmedLeft (skin_.px (30.0f));
        float top = 6.0f;
        for (const float v : history_) top = std::max (top, v);
        top = std::ceil (top / 6.0f) * 6.0f;   // échelle par pas de 6 dB, la réduction descend depuis le haut
        g.setFont (skin_.font (9.0f));
        for (float db = 0.0f; db <= top + 0.01f; db += 6.0f)
        {
            const float y = plot.getY() + db / top * plot.getHeight();
            g.setColour (skin_.colour (Token::Grid, 0.6f));
            g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), std::max (1.0f, skin_.px (1.0f)));
            g.setColour (skin_.colour (Token::TextDim2));
            g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (plot.getX() - skin_.px (30.0f), y - skin_.px (6.0f), skin_.px (24.0f), skin_.px (12.0f)), juce::Justification::centredRight);
        }
        juce::Path area;
        const int n = static_cast<int> (history_.size());
        area.startNewSubPath (plot.getX(), plot.getY());
        for (int i = 0; i < n; ++i)
        {
            const float v = history_[(head_ + static_cast<std::size_t> (i)) % history_.size()];
            area.lineTo (plot.getX() + plot.getWidth() * static_cast<float> (i) / static_cast<float> (n - 1), plot.getY() + std::min (1.0f, v / top) * plot.getHeight());
        }
        area.lineTo (plot.getRight(), plot.getY());
        area.closeSubPath();
        g.setColour (skin_.colour (Token::Accent, 0.55f));
        g.fillPath (area);
        g.setColour (skin_.colour (Token::Accent));
        g.strokePath (area, juce::PathStrokeType (std::max (1.0f, skin_.px (1.5f))));
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (9.0f));
        g.drawText ("dB", juce::Rectangle<float> (plot.getX() - skin_.px (30.0f), plot.getY() - skin_.px (20.0f), skin_.px (24.0f), skin_.px (12.0f)), juce::Justification::centredRight);
    }

private:
    const nova::Skin& skin_;
    std::array<float, 300> history_ {};   // 10 s à 30 Hz
    std::size_t head_ = 0;
};

// ------------------------------------------------------------------------------------------------ éditeur
NovaDigueEditor::NovaDigueEditor (ParamHost& host, const LimiterModule* module, const theme::Palette& palette, theme::KnobStyle knob,
                                  const i18n::Translator& tr, Options options)
    : EditorBase (host, palette, knob, tr, std::move (options), juce::String::fromUTF8 (std::string (tr.tr ("module.limiter")).c_str()).toUpperCase()),
      module_ (module)
{
    addPage (tx ("lm.page_main"), iconPath (0));
    addPage (tx ("nv.page_settings"), iconPath (1));

    metersPanel_ = add<nova::Panel> (-1, skin());
    navPanel_ = add<nova::Panel> (-1, skin());
    navPanel_->toBack();
    viewPanel_ = add<nova::Panel> (0, skin());
    viewPanel_->setTitle (tx ("lm.view_title"));
    view_ = add<HistoryView> (0, skin());
    inMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.in"), nova::VMeter::Kind::Level, false);
    grMeter_ = add<nova::VMeter> (-1, skin(), tx ("lm.gr"), nova::VMeter::Kind::Reduction, true);
    outMeter_ = add<nova::VMeter> (-1, skin(), tx ("nv.out"), nova::VMeter::Kind::Level, false);
    for (auto* m : { inMeter_, grMeter_, outMeter_ }) m->setModeVisible (false);
    grMeter_->setRange (-24.0f, 0.0f);
    inTrimPill_ = add<nova::Pill> (-1, skin(), nova::Pill::Look::Small);
    inTrimPill_->bindValue (host, stock::inputTrim, {});
    inTrimPill_->onClick = [this] { openModal (stock::inputTrim, uistate::Terminal::Pointer); };

    modePill_ = add<nova::Pill> (0, skin());
    modePill_->bindChoice (host, lm::mode, tx ("lm.mode"), { tx ("lm.quality"), tx ("lm.lowlat") });
    addInfo (0, modePill_, "hlp.lm_mode");
    hqPill_ = add<nova::Pill> (0, skin());
    hqPill_->setText (tx ("lm.hq"));
    hqPill_->onClick = [this] { applyHighQuality(); };
    addInfo (0, hqPill_, "hlp.lm_hq");
    latencyText_ = add<nova::Text> (0, skin(), this->options().brand ? Token::TextDim2 : Token::Pink, 11.0f);   // dans un insert de Neptune, la latence n'est pas compensée entre les pistes

    struct Spec { ParamId id; const char* title; bool pink; const char* help; };
    const Spec specs[5] = { { lm::threshold, "lm.threshold", false, "hlp.lm_threshold" }, { lm::ceiling, "lm.ceiling", false, "hlp.lm_ceiling" },
                            { lm::release, "lm.release", false, "hlp.lm_release" }, { lm::lookahead, "lm.lookahead", false, "hlp.lm_lookahead" },
                            { stock::outputTrim, "lm.output", true, "hlp.lm_threshold" } };
    for (const auto& s : specs)
    {
        Column c {};
        c.card = add<nova::Panel> (0, skin());
        c.knob = add<nova::KnobCard> (0, skin(), host, s.id, tx (s.title), modalOpener(), s.pink);
        c.knob->setPlain (true);
        addInfo (0, c.card, s.help, 1);
        columns_.push_back (c);
    }
    buildSettingsPage (1);
    setPage (0);
    setSize (designWidth, designHeight);
}

NovaDigueEditor::~NovaDigueEditor() = default;

bool NovaDigueEditor::isHighQuality() const
{
    return choiceValue (lm::mode) == lm::kQuality && std::fabs (value (lm::lookahead) - lm::maxLookaheadMs) < 1.0e-6;
}

// Réglage de référence mesuré : Quality, 10 ms. Les niveaux (Threshold, Ceiling, Release) ne sont pas touchés.
void NovaDigueEditor::applyHighQuality()
{
    for (const ParamId id : { lm::mode, lm::lookahead })
    {
        host().uiBeginEdit (id);
        setPlain (host(), id, id == lm::mode ? static_cast<double> (lm::kQuality) : lm::maxLookaheadMs);
        host().uiEndEdit (id);
    }
}

void NovaDigueEditor::layout()
{
    metersPanel_->setBounds (R (14, 72, 176, 456));
    {
        const auto in = metersPanel_->getBounds().reduced (skin().ipx (8.0f), skin().ipx (12.0f));
        const int w = in.getWidth() / 3, trimH = skin().ipx (40.0f);
        const auto meters = in.withTrimmedBottom (trimH);
        inMeter_->setBounds (meters.withWidth (w));
        grMeter_->setBounds (meters.withX (meters.getX() + w).withWidth (w));
        outMeter_->setBounds (meters.withX (meters.getX() + 2 * w).withWidth (w));
        inTrimPill_->setBounds (in.getX() + skin().ipx (1.0f), in.getBottom() - skin().ipx (30.0f), w - skin().ipx (2.0f), skin().ipx (24.0f));
    }
    navPanel_->setBounds (R (14, 542, 176, 168));
    layoutNav (542, 168);
    viewPanel_->setBounds (R (204, 72, 942, 456));
    view_->setBounds (R (220, 104, 910, 362));
    modePill_->setBounds (R (220, 480, 200, 32));
    hqPill_->setBounds (R (440, 480, 170, 32));
    latencyText_->setBounds (R (640, 484, 490, 24));
    const float colW = (942.0f - 12.0f * 4.0f) / 5.0f;
    for (std::size_t i = 0; i < columns_.size(); ++i)
    {
        const float x = 204.0f + static_cast<float> (i) * (colW + 12.0f);
        columns_[i].card->setBounds (R (x, 542, colW, 168));
        columns_[i].knob->setBounds (R (x, 542 + 18, colW, 132));
    }
    if (bypassPill() != nullptr) bypassPill()->setBounds (R (1160 - 14 - 36, 11, 36, 36));
}

void NovaDigueEditor::tick()
{
    if (module_ != nullptr)
    {
        inMeter_->update (dbOf (module_->meter (lm::kInPeakL)), dbOf (module_->meter (lm::kInPeakR)));
        outMeter_->update (dbOf (module_->meter (lm::kOutPeakL)), dbOf (module_->meter (lm::kOutPeakR)));
        const float gr = module_->meter (lm::kReduction);
        grMeter_->update (gr, gr);
        if (currentPage() == 0) view_->push (gr);
        const double sr = options().sampleRate ? options().sampleRate() : 48000.0;
        const int latency = module_->latencySamples (host().parameters());
        latencyText_->setText (options().brand ? tx ("lm.latency") + "  " + juce::String (latency) + " smp  (" + juce::String (1000.0 * latency / sr, 1) + " ms)"
                                               : juce::String::fromUTF8 (translator().format ("nv.latency_insert", { std::to_string (latency) }).c_str()));
    }
    hqPill_->setOn (isHighQuality());
}
