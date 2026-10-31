// Galerie des composants NOVA (NovaKit) rendue hors-ligne en PNG, pour comparer avec les maquettes HTML et vérifier l'échelle.
// Usage : NovaGallery <dossier de sortie>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/NovaKit.h"
#include "../src/NovaCompressorEditor.h"
#include "../src/NovaEqualizerEditor.h"
#include "../src/NovaShaperEditor.h"
#include "../src/NovaChorusEditor.h"
#include "../src/NovaDelayEditor.h"
#include "../src/NovaMicrodecaleEditor.h"
#include "../src/NovaDiapasonEditor.h"
#include "../src/NovaMiradorEditor.h"
#include "../src/NovaConvolutionEditor.h"
#include "../src/NovaFournaiseEditor.h"
#include "../src/NovaSynthEditor.h"
#include "../src/NovaFmEditor.h"
#include "../src/NovaSpectrum.h"
#include "neptune/core/EqualizerModule.h"
#include "neptune/core/CompressorModule.h"
#include "neptune/core/ShaperModule.h"
#include "neptune/core/ChorusModule.h"
#include "neptune/core/DelayModule.h"
#include "neptune/core/MicrodecaleModule.h"
#include "neptune/core/SignalGenerator.h"
#include "neptune/core/Mirador.h"
#include "neptune/core/ConvolutionModule.h"
#include "neptune/core/OverdriveModule.h"
#include "neptune/core/StockModule.h"
#include "neptune/core/AnalogSynth.h"
#include "neptune/core/FmSynth.h"
#include "neptune/core/SamplerParams.h"
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <vector>
#include <memory>

using namespace neptune::core;
namespace th = neptune::core::theme;

namespace
{
struct FakeHost : ParamHost
{
    ParameterSet set;
    ParameterSet& parameters() override { return set; }
    void uiBeginEdit (ParamId) override {}
    void uiSetNormalised (ParamId id, double n) override { set.setNormalized (id, n); }
    void uiEndEdit (ParamId) override {}
    void uiResetToDefault (ParamId id) override { set.reset (id); }
};

struct RunnerHost : ParamHost
{
    ModuleRunner& runner;
    explicit RunnerHost (ModuleRunner& r) : runner (r) {}
    ParameterSet& parameters() override { return runner.params(); }
    void uiBeginEdit (ParamId) override {}
    void uiSetNormalised (ParamId id, double n) override { runner.params().setNormalized (id, n); }
    void uiEndEdit (ParamId) override {}
    void uiResetToDefault (ParamId id) override { runner.params().reset (id); }
};

struct EngineHost : ParamHost
{
    generator::GeneratorEngine& engine;
    explicit EngineHost (generator::GeneratorEngine& e) : engine (e) {}
    ParameterSet& parameters() override { return engine.params(); }
    void uiBeginEdit (ParamId) override {}
    void uiSetNormalised (ParamId id, double n) override { engine.params().setNormalized (id, n); }
    void uiEndEdit (ParamId) override {}
    void uiResetToDefault (ParamId id) override { engine.params().reset (id); }
};

struct MiradorHost : ParamHost
{
    mirador::MiradorEngine& engine;
    explicit MiradorHost (mirador::MiradorEngine& e) : engine (e) {}
    ParameterSet& parameters() override { return engine.params(); }
    void uiBeginEdit (ParamId) override {}
    void uiSetNormalised (ParamId id, double n) override { engine.params().setNormalized (id, n); }
    void uiEndEdit (ParamId) override {}
    void uiResetToDefault (ParamId id) override { engine.params().reset (id); }
};

struct FakePresets : PresetHost
{
    preset::Library lib;
    explicit FakePresets (std::filesystem::path root) : lib (std::move (root), analogsynth::factoryPresets()) { lib.open(); }
    preset::Library& library() override { return lib; }
    std::string currentPresetId() const override { return lib.factoryPresets().empty() ? std::string() : lib.factoryPresets().front().id; }
    const preset::Preset* currentPreset() const override { return lib.factoryPresets().empty() ? nullptr : &lib.factoryPresets().front(); }
    std::uint32_t presetRevision() const override { return 1; }
    void touchPresets() override {}
    bool loadPreset (const std::string&) override { return true; }
    bool stepPreset (int) override { return true; }
    bool isModified() const override { return false; }
    bool canSaveCurrent() const override { return false; }
    bool saveCurrent (std::string*) override { return false; }
    bool overwritePreset (const std::string&, std::string*) override { return false; }
    bool saveCurrentAs (const std::string&, const std::string&, preset::Preset*, std::string*) override { return false; }
    bool saveConfig (const preset::LibraryConfig&) override { return true; }
    const preset::LibraryConfig& presetConfig() const override { return lib.config(); }
};

juce::Path iconPath (int kind)
{
    juce::Path p;
    if (kind == 0) p.addRectangle (4.0f, 12.0f, 3.0f, 8.0f), p.addRectangle (10.5f, 5.0f, 3.0f, 15.0f), p.addRectangle (17.0f, 9.0f, 3.0f, 11.0f);
    else if (kind == 1) p.addEllipse (4.0f, 2.0f, 4.0f, 4.0f), p.addEllipse (16.0f, 18.0f, 4.0f, 4.0f), p.startNewSubPath (6.0f, 6.0f), p.lineTo (6.0f, 10.0f), p.lineTo (18.0f, 14.0f), p.lineTo (18.0f, 18.0f);
    else p.addEllipse (8.0f, 8.0f, 8.0f, 8.0f), p.addEllipse (2.0f, 2.0f, 20.0f, 20.0f);
    return p;
}

class Scene : public juce::Component
{
public:
    static constexpr int dw = 1160, dh = 780;
    Scene (const th::Palette& pal, th::KnobStyle ks, th::ThemeId id, float scale, FakeHost& host)
        : skin_ (pal, ks, id), look_ (skin_)
    {
        skin_.setScale (scale);
        setLookAndFeel (&look_);
        const char* names[6] = { "THRESHOLD", "RATIO", "ATTACK", "RELEASE", "KNEE", "MAKEUP" };
        for (int i = 0; i < 6; ++i)
        {
            host.set.set (static_cast<ParamId> (i + 1), host.set.find (static_cast<ParamId> (i + 1))->min
                          + (host.set.find (static_cast<ParamId> (i + 1))->max - host.set.find (static_cast<ParamId> (i + 1))->min) * (0.15 + 0.13 * i));
            auto* k = cards_.add (new nova::KnobCard (skin_, host, static_cast<ParamId> (i + 1), names[i], nullptr, i == 5));
            k->setRangeLabels ("-60", "0");
            addAndMakeVisible (k);
            auto* s = subs_.add (new nova::SubSlider (skin_, host, static_cast<ParamId> (i + 1), "SUB"));
            addAndMakeVisible (s);
        }
        panel_ = std::make_unique<nova::Panel> (skin_);
        panel_->setTitle ("TRANSFER CURVE");
        panel_->setMeta ("IN -18.2 dB  GR 4.1 dB");
        addAndMakeVisible (*panel_);
        meters_ = std::make_unique<nova::Panel> (skin_);
        addAndMakeVisible (*meters_);
        const char* mt[3] = { "IN", "GR", "OUT" };
        for (int i = 0; i < 3; ++i)
        {
            auto* m = vm_.add (new nova::VMeter (skin_, mt[i], i == 1 ? nova::VMeter::Kind::Reduction : nova::VMeter::Kind::Level, i == 2));
            m->update (i == 1 ? 7.0f : -14.0f + 3.0f * static_cast<float> (i), i == 1 ? 6.0f : -17.0f);
            addAndMakeVisible (m);
        }
        const char* nav[3] = { "COMPRESSOR", "SIDECHAIN", "SETTINGS" };
        for (int i = 0; i < 3; ++i)
        {
            auto* n = navs_.add (new nova::NavItem (skin_, nav[i], iconPath (i)));
            n->setActive (i == 0);
            addAndMakeVisible (n);
        }
        for (int i = 0; i < 4; ++i)
        {
            auto* p = pills_.add (new nova::Pill (skin_, i == 3 ? nova::Pill::Look::Icon : nova::Pill::Look::Pill));
            p->setText (i == 0 ? "STEREO" : i == 1 ? "4X" : "LOOKAHEAD 2 ms");
            p->setOn (i == 1);
            if (i == 3) p->setIcon (iconPath (2));
            addAndMakeVisible (p);
        }
        sw_ = std::make_unique<nova::Switch> (skin_);
        sw_->setOn (true);
        addAndMakeVisible (*sw_);
        sw2_ = std::make_unique<nova::Switch> (skin_);
        addAndMakeVisible (*sw2_);
        setSize (juce::roundToInt (dw * scale), juce::roundToInt (dh * scale));
    }
    ~Scene() override { setLookAndFeel (nullptr); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (skin_.colour (th::Token::BgApp));
        nova::paintHeaderBar (g, skin_, header_.toFloat(), "COMPRESSOR", "NOVA SERIES");
        nova::paintBottomBar (g, skin_, bottom_.toFloat());
    }

    void resized() override
    {
        const auto S = [this] (float v) { return skin_.ipx (v); };
        auto r = getLocalBounds();
        header_ = r.removeFromTop (S (58.0f));
        bottom_ = r.removeFromBottom (S (56.0f));
        auto body = r;
        auto side = body.removeFromLeft (S (190.0f)).reduced (S (14.0f), S (14.0f));
        meters_->setBounds (side.removeFromTop (S (396.0f)));
        auto mm = meters_->getBounds().reduced (S (8.0f), S (12.0f));
        const int mw = mm.getWidth() / 3;
        for (int i = 0; i < 3; ++i) vm_[i]->setBounds (mm.removeFromLeft (mw));
        auto navArea = side.withTrimmedTop (S (14.0f));
        for (auto* n : navs_) n->setBounds (navArea.removeFromTop (S (40.0f)).withTrimmedBottom (S (4.0f)));
        auto content = body.reduced (S (14.0f), S (14.0f));
        panel_->setBounds (content.removeFromTop (S (396.0f)));
        content.removeFromTop (S (14.0f));
        const int cw = (content.getWidth() - S (12.0f) * 5) / 6;
        for (int i = 0; i < 6; ++i)
        {
            auto c = content.removeFromLeft (cw);
            content.removeFromLeft (S (12.0f));
            auto card = c.removeFromTop (S (150.0f));
            cards_[i]->setBounds (card);
            subs_[i]->setBounds (c.withTrimmedTop (S (8.0f)).withHeight (S (34.0f)));
        }
        auto b = bottom_.reduced (S (14.0f), S (12.0f));
        for (int i = 0; i < 3; ++i) pills_[i]->setBounds (b.removeFromLeft (S (i == 2 ? 150.0f : 100.0f)).withHeight (S (32.0f))), b.removeFromLeft (S (12.0f));
        sw_->setBounds (b.removeFromLeft (S (34.0f)).withSizeKeepingCentre (S (34.0f), S (19.0f))), b.removeFromLeft (S (12.0f));
        sw2_->setBounds (b.removeFromLeft (S (34.0f)).withSizeKeepingCentre (S (34.0f), S (19.0f)));
        pills_[3]->setBounds (bottom_.getRight() - S (14.0f + 36.0f), bottom_.getCentreY() - S (18.0f), S (36.0f), S (36.0f));
        for (auto* k : cards_) k->resized();
    }

private:
    nova::Skin skin_;
    nova::LookAndFeel look_;
    juce::OwnedArray<nova::KnobCard> cards_;
    juce::OwnedArray<nova::SubSlider> subs_;
    juce::OwnedArray<nova::VMeter> vm_;
    juce::OwnedArray<nova::NavItem> navs_;
    juce::OwnedArray<nova::Pill> pills_;
    std::unique_ptr<nova::Panel> panel_, meters_;
    std::unique_ptr<nova::Switch> sw_, sw2_;
    juce::Rectangle<int> header_, bottom_;
};
} // namespace

static float gWide = 1.0f;   // NOVA_GALLERY_WIDE=1.5 : fenêtres élargies de moitié (étirement horizontal), sorties à mettre dans un autre dossier

int main (int argc, char** argv)
{
    if (const char* w = std::getenv ("NOVA_GALLERY_WIDE")) gWide = std::max (1.0f, static_cast<float> (std::atof (w)));
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "nova-gallery");
    out.createDirectory();

    FakeHost host;
    for (int i = 0; i < 6; ++i)
        host.set.add (ParamSpec::continuous (static_cast<ParamId> (i + 1), "p" + std::to_string (i), i % 2 ? Unit::Milliseconds : Unit::Decibel, i % 2 ? 1.0 : -60.0, i % 2 ? 500.0 : 0.0, i % 2 ? 10.0 : -12.0));

    struct Case { const char* name; th::ThemeId id; float scale; };
    const Case cases[] = { { "dark-100", th::ThemeId::Dark, 1.0f }, { "light-100", th::ThemeId::Light, 1.0f },
                           { "hc-100", th::ThemeId::HighContrast, 1.0f }, { "hammond-100", th::ThemeId::Hammond, 1.0f },
                           { "dark-070", th::ThemeId::Dark, 0.7f }, { "dark-140", th::ThemeId::Dark, 1.4f } };
    for (const auto& c : cases)
    {
        std::fprintf (stderr, "case %s\n", c.name);
        const auto& def = th::definition (c.id);
        Scene scene (th::withContrast (def, 0), def.knob, c.id, c.scale, host);
        const auto img = scene.createComponentSnapshot (scene.getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
        if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
        std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
    }
    // ---- l'éditeur réel du compresseur : trois pages, quatre thèmes, plusieurs échelles
    ModuleRunner runner (std::make_unique<CompressorModule>());
    runner.prepare (48000.0, 512);
    RunnerHost rhost (runner);
    auto& ps = runner.params();
    ps.set (compressor::threshold, -24.0);
    ps.set (compressor::ratio, 4.0);
    ps.set (compressor::attack, 10.0);
    ps.set (compressor::release, 150.0);
    ps.set (compressor::makeup, 3.2);
    ps.set (compressor::sideEqBypass, 0.0);
    ps.set (compressor::bandParam (0, compressor::kStatus), compressor::kOn);
    ps.set (compressor::bandParam (0, compressor::kType), compressor::kHighPass);
    ps.set (compressor::bandParam (0, compressor::kFreq), 120.0);
    ps.set (compressor::bandParam (1, compressor::kStatus), compressor::kOn);
    ps.set (compressor::bandParam (1, compressor::kFreq), 2500.0);
    ps.set (compressor::bandParam (1, compressor::kGain), 6.0);
    ps.set (compressor::bandParam (1, compressor::kQ), 1.2);
    ps.set (compressor::bandParam (2, compressor::kStatus), compressor::kOn);
    ps.set (compressor::bandParam (2, compressor::kFreq), 400.0);
    ps.set (compressor::bandParam (2, compressor::kGain), -5.0);
    ps.set (compressor::bandParam (4, compressor::kStatus), compressor::kOn);
    ps.set (compressor::bandParam (4, compressor::kType), compressor::kLowPass);
    ps.set (compressor::bandParam (4, compressor::kFreq), 9000.0);
    const i18n::Translator tr ("en");
    struct EdCase { const char* name; th::ThemeId id; float scale; int page; };
    const EdCase eds[] = { { "comp-dark-p0", th::ThemeId::Dark, 1.0f, 0 }, { "comp-dark-p3", th::ThemeId::Dark, 1.0f, 3 }, { "comp-dark-p1", th::ThemeId::Dark, 1.0f, 1 }, { "comp-dark-p2", th::ThemeId::Dark, 1.0f, 2 },
                           { "comp-light-p0", th::ThemeId::Light, 1.0f, 0 }, { "comp-hc-p0", th::ThemeId::HighContrast, 1.0f, 0 },
                           { "comp-hammond-p1", th::ThemeId::Hammond, 1.0f, 1 }, { "comp-dark-p0-070", th::ThemeId::Dark, 0.7f, 0 } };
    for (const auto& c : eds)
    {
        std::fprintf (stderr, "editor %s\n", c.name);
        const auto& def = th::definition (c.id);
        NovaCompressorEditor::Options opt;
        opt.brand = true;
        opt.moduleName = "compressor";
        opt.stretchAuto = gWide > 1.0f;
        opt.getSettings = [] { return settings::GlobalSettings{}; };
        opt.applySettings = [] (const settings::GlobalSettings&) {};
        NovaCompressorEditor ed (rhost, static_cast<const CompressorModule*> (&runner.module()), th::withContrast (def, 0), def.knob, tr, opt);
        ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
        ed.setPageForTest (c.page);
        // signal simulé : percussions toutes les 0,6 s, jouées à 30 images par seconde pendant 10 s ; l'historique défile sur l'horloge simulée
        {
            std::vector<float> l (512), r (512);
            long sampleIndex = 0;
            double simTime = 100.0;
            for (int frame = 0; frame < 300; ++frame)
            {
                int remaining = 1600;
                while (remaining > 0)
                {
                    const int n = std::min (512, remaining);
                    for (int i = 0; i < n; ++i, ++sampleIndex)
                    {
                        const double t = static_cast<double> (sampleIndex) / 48000.0;
                        const double since = std::fmod (t, 0.6);
                        const double amp = 0.08 + 0.75 * std::exp (-since * 7.0) * (std::fmod (t, 2.4) < 0.6 ? 0.55 : 1.0);
                        const float v = static_cast<float> (amp * std::sin (6.2831853 * 110.0 * t));
                        l[static_cast<std::size_t> (i)] = v;
                        r[static_cast<std::size_t> (i)] = v * 0.9f;
                    }
                    runner.process (l.data(), r.data(), n, false);
                    remaining -= n;
                }
                simTime += 1.0 / 30.0;
                ed.refreshNow();
                ed.stepHistoryForTest (simTime);
            }
        }
        const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
        if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
        std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
    }

    // ---- l'éditeur réel de La Flèche (shaper)
    {
        ModuleRunner shRunner (std::make_unique<ShaperModule>());
        shRunner.prepare (48000.0, 512);
        RunnerHost sh (shRunner);
        auto& sp = shRunner.params();
        sp.set (shaper::attackGain1, 60.0);
        sp.set (shaper::sustainGain1, -35.0);
        sp.set (shaper::scEnable, 1.0);
        sp.set (shaper::scBand2Gain, 5.0);
        struct ShCase { const char* name; th::ThemeId id; float scale; int page; bool response; };
        const ShCase scs[] = { { "shaper-dark-p0", th::ThemeId::Dark, 1.0f, 0, false }, { "shaper-dark-resp", th::ThemeId::Dark, 1.0f, 0, true }, { "shaper-dark-p1", th::ThemeId::Dark, 1.0f, 1, false },
                               { "shaper-dark-p2", th::ThemeId::Dark, 1.0f, 2, false }, { "shaper-dark-p3", th::ThemeId::Dark, 1.0f, 3, false },
                               { "shaper-light-p0", th::ThemeId::Light, 1.0f, 0, false }, { "shaper-hammond-p1", th::ThemeId::Hammond, 1.0f, 1, false },
                               { "shaper-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, false } };
        for (const auto& c : scs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaShaperEditor::Options opt;
            opt.brand = true;
            opt.externalKey = true;
            opt.moduleName = "shaper";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaShaperEditor ed (sh, static_cast<const ShaperModule*> (&shRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.showResponseForTest (c.response);
            ed.refreshNow();   // active l'anneau d'analyse
            // signal simulé : percussions toutes les 0,6 s, jouées à 30 images par seconde pendant 10 s ; l'historique défile sur l'horloge simulée
            {
                std::vector<float> l (512), r (512);
                long sampleIndex = 0;
                double simTime = 100.0;
                ed.stepWaveForTest (simTime);
                for (int frame = 0; frame < 300; ++frame)
                {
                    int remaining = 1600;
                    while (remaining > 0)
                    {
                        const int n = std::min (512, remaining);
                        for (int i = 0; i < n; ++i, ++sampleIndex)
                        {
                            const double t = static_cast<double> (sampleIndex) / 48000.0;
                            const double since = std::fmod (t, 0.6);
                            const double amp = 0.08 + 0.75 * std::exp (-since * 7.0) * (std::fmod (t, 2.4) < 0.6 ? 0.55 : 1.0);
                            const float v = static_cast<float> (amp * std::sin (6.2831853 * 110.0 * t));
                            l[static_cast<std::size_t> (i)] = v;
                            r[static_cast<std::size_t> (i)] = v * 0.9f;
                        }
                        shRunner.process (l.data(), r.data(), n, false);
                        remaining -= n;
                    }
                    simTime += 1.0 / 30.0;
                    ed.refreshNow();
                    ed.stepWaveForTest (simTime);
                }
            }
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel du delay
    {
        ModuleRunner dRunner (std::make_unique<DelayModule>());
        dRunner.prepare (48000.0, 512);
        RunnerHost dh (dRunner);
        auto& dp = dRunner.params();
        namespace dy = delay;
        auto line = [&] (int n, double ms, double lv, double pan, double fb)
        {
            dp.set (dy::lineParam (n, dy::kOn), 1.0);
            dp.set (dy::lineParam (n, dy::kTime), ms);
            dp.set (dy::lineParam (n, dy::kLevel), lv);
            dp.set (dy::lineParam (n, dy::kPan), pan);
            dp.set (dy::lineParam (n, dy::kFeedback), fb);
        };
        line (0, 190, 0, -30, 45);
        line (1, 380, -4, 30, 40);
        line (2, 570, -8, 0, 50);
        line (3, 900, -12, 60, 30);
        dp.set (dy::lineParam (2, dy::kHighCut), 4000.0);
        dp.set (dy::lineParam (2, dy::kLowCut), 150.0);
        struct DCase { const char* name; th::ThemeId id; float scale; int page; bool filters; int sel; };
        const DCase dcs[] = { { "delay-dark-p0", th::ThemeId::Dark, 1.0f, 0, false, 2 }, { "delay-dark-filters", th::ThemeId::Dark, 1.0f, 0, true, 2 }, { "delay-dark-p1", th::ThemeId::Dark, 1.0f, 1, false, 2 },
                              { "delay-dark-p2", th::ThemeId::Dark, 1.0f, 2, false, 0 }, { "delay-dark-p3", th::ThemeId::Dark, 1.0f, 3, false, 0 },
                              { "delay-light-p0", th::ThemeId::Light, 1.0f, 0, false, 0 }, { "delay-hammond-p0", th::ThemeId::Hammond, 1.0f, 0, false, 1 },
                              { "delay-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, false, 2 } };
        for (const auto& c : dcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaDelayEditor::Options opt;
            opt.brand = true;
            opt.hostTempo = true;
            opt.moduleName = "delay";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaDelayEditor ed (dh, static_cast<const DelayModule*> (&dRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.selectLineForTest (c.sel);
            ed.showFiltersForTest (c.filters);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel du chorus
    {
        ModuleRunner cRunner (std::make_unique<ChorusModule>());
        cRunner.prepare (48000.0, 512);
        RunnerHost chh (cRunner);
        auto& cp = cRunner.params();
        cp.set (chorus::voices, 1.0);
        cp.set (chorus::feedback, 20.0);
        cp.set (chorus::lowCutOn, 1.0);
        cp.set (chorus::lowCut, 180.0);
        cp.set (chorus::highCutOn, 1.0);
        cp.set (chorus::highCut, 9000.0);
        cp.set (chorus::bbd, 60.0);
        struct CCase { const char* name; th::ThemeId id; float scale; int page; bool response; bool tal; };
        const CCase ccs[] = { { "chorus-dark-p0", th::ThemeId::Dark, 1.0f, 0, false, false }, { "chorus-dark-resp", th::ThemeId::Dark, 1.0f, 0, true, false }, { "chorus-dark-tal", th::ThemeId::Dark, 1.0f, 0, false, true },
                              { "chorus-dark-p1", th::ThemeId::Dark, 1.0f, 1, false, false }, { "chorus-dark-p2", th::ThemeId::Dark, 1.0f, 2, false, false },
                              { "chorus-light-p0", th::ThemeId::Light, 1.0f, 0, false, false }, { "chorus-hammond-p0", th::ThemeId::Hammond, 1.0f, 0, false, false },
                              { "chorus-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, false, false } };
        for (const auto& c : ccs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            cp.set (chorus::mode, c.tal ? 1.0 : 0.0);
            cp.set (chorus::talI, c.tal ? 1.0 : 0.0);
            cp.set (chorus::talII, c.tal ? 1.0 : 0.0);
            const auto& def = th::definition (c.id);
            NovaChorusEditor::Options opt;
            opt.brand = true;
            opt.hostTempo = true;
            opt.moduleName = "chorus";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaChorusEditor ed (chh, static_cast<const ChorusModule*> (&cRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.showResponseForTest (c.response);
            {
                std::vector<float> l (512, 0.0f), r (512, 0.0f);
                for (int b = 0; b < 40; ++b) cRunner.process (l.data(), r.data(), 512, false);   // fait avancer la phase du LFO
            }
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel d'AnalogSynth
    {
        analogsynth::AnalogSynthEngine engine;
        engine.prepare (48000.0, 512);
        struct SHost : ParamHost
        {
            ParameterSet& set;
            explicit SHost (ParameterSet& s) : set (s) {}
            ParameterSet& parameters() override { return set; }
            void uiBeginEdit (ParamId) override {}
            void uiSetNormalised (ParamId id, double n) override { set.setNormalized (id, n); }
            void uiEndEdit (ParamId) override {}
            void uiResetToDefault (ParamId id) override { set.reset (id); }
        } sh (engine.params());
        FakePresets presets (std::filesystem::path (out.getChildFile ("synth-presets").getFullPathName().toStdString()));
        SynthDescription desc;
        desc.osc[0].wave = kWaveSaw;
        desc.osc[0].unison = 5;
        desc.osc[1].wave = kWavePulse;
        desc.osc[1].octave = -1;
        desc.osc[1].sync = true;
        desc.osc[2].wave = kWaveSine;
        desc.filters.filter[1].on = true;
        desc.filters.parallel = true;
        desc.ampEnv.attack.ms = 40.0;
        desc.ampEnv.decay.ms = 300.0;
        desc.ampEnv.sustain = 0.6;
        desc.filters.filter[0].env.decay.ms = 500.0;
        desc.filters.filter[0].envDepthOctaves = 3.0;
        engine.setDescription (desc);
        struct SCase { const char* name; th::ThemeId id; float scale; int page; bool keyboard; };
        const SCase scs[] = { { "synth-dark-p0", th::ThemeId::Dark, 1.0f, 0, true }, { "synth-dark-p0-nokb", th::ThemeId::Dark, 1.0f, 0, false }, { "synth-dark-p1", th::ThemeId::Dark, 1.0f, 1, true },
                              { "synth-dark-p2", th::ThemeId::Dark, 1.0f, 2, true }, { "synth-dark-p3", th::ThemeId::Dark, 1.0f, 3, true }, { "synth-dark-p4", th::ThemeId::Dark, 1.0f, 4, true },
                              { "synth-light-p0", th::ThemeId::Light, 1.0f, 0, true }, { "synth-hammond-p1", th::ThemeId::Hammond, 1.0f, 1, false }, { "synth-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, true } };
        for (const auto& c : scs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaSynthEditor::Options opt;
            opt.brand = true;
            opt.noBypass = true;
            opt.moduleName = "analogsynth";
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            SynthLink link;
            link.description = [&desc] { return desc; };
            link.edit = [] (SynthDescription) {};
            link.heldKeys = [] (int) { return std::uint64_t (0); };
            link.outputs = { analogsynth::volume, analogsynth::pan, static_cast<ParamId> (sampler::gain), static_cast<ParamId> (sampler::tuneSemitones), static_cast<ParamId> (sampler::tuneCents) };
            NovaSynthEditor ed (sh, presets, link, th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.setKeyboardShownForTest (c.keyboard);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de FM6Op
    {
        fm::FmSynthEngine fmEngine;
        fmEngine.prepare (48000.0, 512);
        struct FHost : ParamHost
        {
            ParameterSet& set;
            explicit FHost (ParameterSet& s) : set (s) {}
            ParameterSet& parameters() override { return set; }
            void uiBeginEdit (ParamId) override {}
            void uiSetNormalised (ParamId id, double n) override { set.setNormalized (id, n); }
            void uiEndEdit (ParamId) override {}
            void uiResetToDefault (ParamId id) override { set.reset (id); }
        } fh (fmEngine.params());
        FakePresets fpresets (std::filesystem::path (out.getChildFile ("fm-presets").getFullPathName().toStdString()));
        fm::FmDescription fdesc;
        fdesc.patch.setGlobal (fm::kAlgorithmOffset, 4);
        fdesc.patch.setGlobal (fm::kFeedbackOffset, 5);
        fdesc.patch.setName ("E.PIANO 1");
        for (int op = 0; op < fm::kOperators; ++op)
        {
            fdesc.patch.setOperatorOn (op, true);
            fdesc.patch.setOp (op, fm::kOutputLevel, 60 + op * 6);
            fdesc.patch.setOp (op, fm::kRate1, 90 - op * 5);
            fdesc.patch.setOp (op, fm::kRate2, 70);
            fdesc.patch.setOp (op, fm::kRate3, 40);
            fdesc.patch.setOp (op, fm::kRate4, 60);
            fdesc.patch.setOp (op, fm::kLevel1, 99);
            fdesc.patch.setOp (op, fm::kLevel2, 80 - op * 4);
            fdesc.patch.setOp (op, fm::kLevel3, 50);
            fdesc.patch.setOp (op, fm::kLevel4, 0);
            fdesc.patch.setOp (op, fm::kCoarse, 1 + op);
            fdesc.patch.setOp (op, fm::kLeftDepth, 20);
            fdesc.patch.setOp (op, fm::kRightDepth, 35);
            fdesc.patch.setOp (op, fm::kLeftCurve, 0);
            fdesc.patch.setOp (op, fm::kRightCurve, 2);
        }
        fmEngine.setDescription (fdesc);
        struct FCase { const char* name; th::ThemeId id; float scale; int page; bool keyboard; };
        const FCase fcs[] = { { "fm-dark-p0", th::ThemeId::Dark, 1.0f, 0, true }, { "fm-dark-p0-nokb", th::ThemeId::Dark, 1.0f, 0, false }, { "fm-dark-p1", th::ThemeId::Dark, 1.0f, 1, true },
                              { "fm-dark-p2", th::ThemeId::Dark, 1.0f, 2, true }, { "fm-light-p0", th::ThemeId::Light, 1.0f, 0, true }, { "fm-hammond-p1", th::ThemeId::Hammond, 1.0f, 1, false },
                              { "fm-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, true } };
        for (const auto& c : fcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaFmEditor::Options opt;
            opt.brand = true;
            opt.noBypass = true;
            opt.moduleName = "fm6op";
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            FmLink link;
            link.description = [&fdesc] { return fdesc; };
            link.edit = [] (fm::FmDescription, std::string, std::string) {};
            link.heldKeys = [] (int) { return std::uint64_t (0); };
            link.outputs = { fm::synthVolume, fm::synthPan, static_cast<ParamId> (sampler::gain), static_cast<ParamId> (sampler::tuneSemitones), static_cast<ParamId> (sampler::tuneCents) };
            NovaFmEditor ed (fh, fpresets, link, th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.setKeyboardShownForTest (c.keyboard);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de Microdécale
    {
        ModuleRunner mRunner (std::make_unique<MicrodecaleModule>());
        mRunner.prepare (48000.0, 512);
        RunnerHost mh (mRunner);
        auto& mp = mRunner.params();
        mp.set (microdecale::feedbackL, 30.0);
        mp.set (microdecale::levelDryL, 40.0);
        struct MCase { const char* name; th::ThemeId id; float scale; int page; };
        const MCase mcs[] = { { "micro-dark-p0", th::ThemeId::Dark, 1.0f, 0 }, { "micro-dark-p1", th::ThemeId::Dark, 1.0f, 1 }, { "micro-dark-p2", th::ThemeId::Dark, 1.0f, 2 },
                              { "micro-light-p0", th::ThemeId::Light, 1.0f, 0 }, { "micro-hammond-p1", th::ThemeId::Hammond, 1.0f, 1 }, { "micro-dark-p0-070", th::ThemeId::Dark, 0.7f, 0 } };
        for (const auto& c : mcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaMicrodecaleEditor::Options opt;
            opt.brand = true;
            opt.moduleName = "microdecale";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaMicrodecaleEditor ed (mh, static_cast<const MicrodecaleModule*> (&mRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de Diapason
    {
        generator::GeneratorEngine dEngine;
        dEngine.prepare (48000.0, 512);
        EngineHost dh (dEngine);
        auto& dp = dEngine.params();
        dp.set (generator::steps, 16);
        for (const int s : { 0, 8, 10 }) dp.set (generator::stepParam (generator::kKick, s), s == 0 ? 2 : 1);
        for (const int s : { 4, 12 }) dp.set (generator::stepParam (generator::kSnare, s), 2);
        for (int s = 0; s < 16; s += 2) dp.set (generator::stepParam (s == 14 ? generator::kHatOpen : generator::kHatClosed, s), s % 4 == 0 ? 2 : 1);
        struct DCase { const char* name; th::ThemeId id; float scale; int page; int signal; int mode; };
        const DCase dcs[] = { { "diapason-dark-sine", th::ThemeId::Dark, 1.0f, 0, generator::kSine, 0 }, { "diapason-dark-square", th::ThemeId::Dark, 1.0f, 0, generator::kSquare, 1 },
                              { "diapason-dark-pink", th::ThemeId::Dark, 1.0f, 0, generator::kPink, 0 }, { "diapason-dark-sweep", th::ThemeId::Dark, 1.0f, 0, generator::kSweepLog, 2 },
                              { "diapason-dark-two", th::ThemeId::Dark, 1.0f, 0, generator::kTwoTone, 0 }, { "diapason-dark-drums", th::ThemeId::Dark, 1.0f, 1, 0, 0 },
                              { "diapason-light-drums", th::ThemeId::Light, 1.0f, 1, 0, 0 }, { "diapason-dark-set", th::ThemeId::Dark, 1.0f, 2, 0, 0 },
                              { "diapason-dark-sine-070", th::ThemeId::Dark, 0.7f, 0, generator::kSine, 0 } };
        for (const auto& c : dcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            dp.set (generator::signal, c.signal);
            dp.set (generator::mode, c.mode);
            NovaDiapasonEditor::Options opt;
            opt.brand = true;
            opt.moduleName = "diapason";
            opt.stretchAuto = gWide > 1.0f;
            opt.noPresets = true;
            opt.noAB = true;
            opt.noBypass = true;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaDiapasonEditor ed (dh, dEngine, th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.selectDrumForTest (2);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de Convolution (IR intégrée « hall »)
    {
        ModuleRunner cRunner (std::make_unique<ConvolutionModule>());
        cRunner.prepare (48000.0, 512);
        RunnerHost ch (cRunner);
        static_cast<ConvolutionModule&> (cRunner.module()).setIrPath ("builtin:hall");
        std::this_thread::sleep_for (std::chrono::milliseconds (600));   // le fil de chargement calcule l'IR
        struct CCase { const char* name; th::ThemeId id; float scale; int page; };
        const CCase ccs[] = { { "conv-dark-p0", th::ThemeId::Dark, 1.0f, 0 }, { "conv-dark-p1", th::ThemeId::Dark, 1.0f, 1 }, { "conv-dark-p2", th::ThemeId::Dark, 1.0f, 2 },
                              { "conv-light-p0", th::ThemeId::Light, 1.0f, 0 } };
        for (const auto& c : ccs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaConvolutionEditor::Options opt;
            opt.brand = true;
            opt.moduleName = "convolution";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaConvolutionEditor ed (ch, static_cast<ConvolutionModule*> (&cRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de Fournaise
    {
        ModuleRunner fRunner (std::make_unique<OverdriveModule>());
        fRunner.prepare (48000.0, 512);
        RunnerHost fh (fRunner);
        auto& fp = fRunner.params();
        fp.set (overdrive::drive, 24.0);
        fp.set (overdrive::bias, 25.0);
        fp.set (overdrive::eqOn, 1.0);
        fp.set (overdrive::bandParam (0, overdrive::kOn), 1.0);
        fp.set (overdrive::bandParam (0, overdrive::kGain), 4.0);
        fp.set (overdrive::bandParam (1, overdrive::kOn), 1.0);
        fp.set (overdrive::bandParam (1, overdrive::kGain), -5.0);
        fp.set (overdrive::bandParam (1, overdrive::kFreq), 900.0);
        fp.set (overdrive::bandParam (2, overdrive::kOn), 1.0);
        fp.set (overdrive::bandParam (2, overdrive::kGain), 3.0);
        fp.set (overdrive::inHpOn, 1.0);
        fp.set (overdrive::outLpOn, 1.0);
        fp.set (overdrive::keySource, 1.0);
        fp.set (overdrive::keyAmount, 40.0);
        struct FCase { const char* name; th::ThemeId id; float scale; int page; int model; bool insert = false; };
        const FCase fcs[] = { { "fournaise-dark-tube", th::ThemeId::Dark, 1.0f, 0, 0 }, { "fournaise-dark-fuzz", th::ThemeId::Dark, 1.0f, 0, 3 },
                              { "fournaise-light-tape", th::ThemeId::Light, 1.0f, 0, 1 }, { "fournaise-dark-filters", th::ThemeId::Dark, 1.0f, 1, 0 },
                              { "fournaise-dark-eq", th::ThemeId::Dark, 1.0f, 2, 0 }, { "fournaise-dark-key", th::ThemeId::Dark, 1.0f, 3, 0 },
                              { "fournaise-dark-set", th::ThemeId::Dark, 1.0f, 4, 0 }, { "fournaise-dark-tube-070", th::ThemeId::Dark, 0.7f, 0, 0 },
                              { "fournaise-insert-os", th::ThemeId::Dark, 1.0f, 0, 0, true } };
        for (const auto& c : fcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            fp.set (overdrive::model, c.model);
            fp.set (overdrive::oversampling, c.insert ? 1.0 : 2.0);   // dans un insert : 2x pour voir l'annonce de latence
            NovaFournaiseEditor::Options opt;
            opt.brand = ! c.insert;
            opt.externalKey = ! c.insert;
            opt.moduleName = "fournaise";
            opt.noPresets = false;
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaFournaiseEditor ed (fh, static_cast<const OverdriveModule*> (&fRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.setPageForTest (c.page);
            // un peu de signal pour les mètres, le point de la courbe de transfert et la clé
            std::vector<float> l (512), r (512);
            double ph = 0.0;
            for (int blk = 0; blk < 12; ++blk)
            {
                for (std::size_t i = 0; i < 512; ++i) { l[i] = r[i] = static_cast<float> (0.3 * std::sin (ph)); ph += 2.0 * 3.14159265358979 * 220.0 / 48000.0; }
                fRunner.process (l.data(), r.data(), 512, false);
            }
            ed.refreshNow();
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }

    // ---- l'éditeur réel de Mirador
    {
        nova::SpectrumEngine::setThreading (false);
        mirador::MiradorEngine mEngine;
        mEngine.prepare (48000.0, 480);
        MiradorHost mh (mEngine);
        struct MCase { const char* name; th::ThemeId id; float scale; int page; float widthMul; };
        const MCase mcs[] = { { "mirador-dark-loud", th::ThemeId::Dark, 1.0f, 0, 1.0f }, { "mirador-dark-levels", th::ThemeId::Dark, 1.0f, 1, 1.0f },
                              { "mirador-dark-stereo", th::ThemeId::Dark, 1.0f, 2, 1.0f }, { "mirador-dark-spectrum", th::ThemeId::Dark, 1.0f, 3, 1.0f },
                              { "mirador-light-loud", th::ThemeId::Light, 1.0f, 0, 1.0f }, { "mirador-dark-set", th::ThemeId::Dark, 1.0f, 4, 1.0f },
                              { "mirador-dark-loud-070", th::ThemeId::Dark, 0.7f, 0, 1.0f }, { "mirador-dark-loud-wide", th::ThemeId::Dark, 1.0f, 0, 1.5f },
                              { "mirador-dark-levels-wide", th::ThemeId::Dark, 1.0f, 1, 1.5f }, { "mirador-dark-stereo-wide", th::ThemeId::Dark, 1.0f, 2, 1.5f },
                              { "mirador-dark-set-wide", th::ThemeId::Dark, 1.0f, 4, 1.5f } };
        for (const auto& c : mcs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaMiradorEditor::Options opt;
            opt.brand = true;
            opt.moduleName = "mirador";
            opt.stretchWidth = true;
            opt.noPresets = true;
            opt.noAB = true;
            opt.noBypass = true;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaMiradorEditor ed (mh, mEngine, th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * c.widthMul), juce::roundToInt (780 * c.scale));
            if (c.widthMul > 1.0f && c.page == 0) ed.openLegendForTest();
            ed.setPageForTest (c.page);
            // un signal stéréo un peu large (sinus gauche + sinus droit décalé + bruit) pour que les mesures et le dessin aient de la matière
            std::vector<float> l (480), r (480);
            double ph = 0.0, ph2 = 0.0;
            std::uint32_t seed = 2463534242u;
            auto feed = [&] (int blocks)
            {
                for (int blk = 0; blk < blocks; ++blk)
                {
                    for (std::size_t i = 0; i < 480; ++i)
                    {
                        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                        const float n = static_cast<float> (static_cast<std::int32_t> (seed)) * (1.0f / 2147483648.0f);
                        l[i] = static_cast<float> (0.25 * std::sin (ph) + 0.08 * std::sin (ph * 5.0)) + 0.03f * n;
                        r[i] = static_cast<float> (0.22 * std::sin (ph2) + 0.08 * std::sin (ph * 5.0)) - 0.03f * n;
                        ph += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
                        ph2 += 2.0 * 3.14159265358979 * 223.0 / 48000.0;
                    }
                    mEngine.process (l.data(), r.data(), 480, true);
                    if (blk % 10 == 9) std::this_thread::sleep_for (std::chrono::milliseconds (100));
                }
            };
            feed (40);
            ed.refreshNow();   // le spectre prépare son analyseur au premier passage : il lui faut du signal après
            feed (60);
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            ed.refreshNow();
            ed.refreshNow();
            if (c.page == 0)
            {
                // page SONIE : de quoi remplir une partie de l'historique (une minute d'historique, 10 blocs = 0,1 s d'audio par passage)
                mh.uiSetNormalised (mirador::historyMinutes, mEngine.params().spec (mEngine.params().indexOf (mirador::historyMinutes)).toNormalized (1.0));
                for (int k = 0; k < 150; ++k) { feed (10); ed.refreshNow(); }
            }
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
        mEngine.stop();
    }

    // ---- l'éditeur réel de l'égaliseur
    {
        nova::SpectrumEngine::setThreading (false);
        ModuleRunner eqRunner (std::make_unique<EqualizerModule>());
        eqRunner.prepare (48000.0, 512);
        RunnerHost eh (eqRunner);
        auto& ep = eqRunner.params();
        namespace e = equalizer;
        auto band = [&] (int b, int type, double f, double g, double q, int dyn = 0)
        {
            ep.set (e::bandParam (b, e::kStatus), e::kOn);
            ep.set (e::bandParam (b, e::kType), type);
            ep.set (e::bandParam (b, e::kFreq), f);
            ep.set (e::bandParam (b, e::kGain), g);
            ep.set (e::bandParam (b, e::kQ), q);
            ep.set (e::bandParam (b, e::kDynOn), dyn);
            ep.set (e::bandParam (b, e::kTarget), -6.0);
            ep.set (e::bandParam (b, e::kThreshold), -30.0);
        };
        ep.set (e::spOn, 1.0);
        band (0, e::kHighPass, 80, 0, 0.707);
        band (1, e::kPeak, 250, -3.5, 1.2);
        band (2, e::kPeak, 3200, 4.0, 0.9, 1);
        band (3, e::kHighShelf, 9000, 2.5, 0.7);
        band (4, e::kNotch, 1800, 0, 4.0);
        ep.set (e::bandParam (2, e::kSideType), e::kSideBandPass);
        {
            std::vector<float> l (512), r (512);
            for (int b = 0; b < 40; ++b)
            {
                for (int i = 0; i < 512; ++i)
                    l[static_cast<std::size_t> (i)] = r[static_cast<std::size_t> (i)] = 0.4f * std::sin (0.3f * static_cast<float> (b * 512 + i));
                eqRunner.process (l.data(), r.data(), 512, false);
            }
        }
        struct EqCase { const char* name; th::ThemeId id; float scale; int page; int sel; };
        const EqCase ecs[] = { { "eq-dark-p0", th::ThemeId::Dark, 1.0f, 0, 2 }, { "eq-dark-p2", th::ThemeId::Dark, 1.0f, 2, 2 }, { "eq-dark-p1", th::ThemeId::Dark, 1.0f, 1, 2 }, { "eq-light-p0", th::ThemeId::Light, 1.0f, 0, 0 },
                               { "eq-hc-p0", th::ThemeId::HighContrast, 1.0f, 0, 1 }, { "eq-hammond-p0", th::ThemeId::Hammond, 1.0f, 0, 3 }, { "eq-dark-p0-070", th::ThemeId::Dark, 0.7f, 0, 2 } };
        for (const auto& c : ecs)
        {
            std::fprintf (stderr, "editor %s\n", c.name);
            const auto& def = th::definition (c.id);
            NovaEqualizerEditor::Options opt;
            opt.brand = true;
            opt.moduleName = "equalizer";
            opt.stretchAuto = gWide > 1.0f;
            opt.getSettings = [] { return settings::GlobalSettings{}; };
            opt.applySettings = [] (const settings::GlobalSettings&) {};
            NovaEqualizerEditor ed (eh, static_cast<const EqualizerModule*> (&eqRunner.module()), th::withContrast (def, 0), def.knob, tr, opt);
            ed.setSize (juce::roundToInt (1160 * c.scale * gWide), juce::roundToInt (780 * c.scale));
            ed.selectBandForTest (c.sel);
            ed.setPageForTest (c.page);
            ed.refreshNow();   // active l'anneau d'analyse
            // signal simulé (bruit passé dans un filtre d'un pôle, deux sinus) joué à 30 images par seconde ; le spectre est calculé sans fil
            {
                std::vector<float> l (512), r (512);
                float lp = 0.0f;
                std::uint32_t seed = 12345u;
                long sampleIndex = 0;
                for (int frame = 0; frame < 150; ++frame)
                {
                    int remaining = 1600;
                    while (remaining > 0)
                    {
                        const int n = std::min (512, remaining);
                        for (int i = 0; i < n; ++i, ++sampleIndex)
                        {
                            seed = seed * 1664525u + 1013904223u;
                            const float w = static_cast<float> (seed >> 8) / 8388608.0f - 1.0f;
                            lp += 0.18f * (w - lp);
                            const double t = static_cast<double> (sampleIndex) / 48000.0;
                            const float v = 0.5f * lp + 0.12f * static_cast<float> (std::sin (6.2831853 * 220.0 * t)) + 0.05f * static_cast<float> (std::sin (6.2831853 * 3000.0 * t));
                            l[static_cast<std::size_t> (i)] = v;
                            r[static_cast<std::size_t> (i)] = 0.9f * v;
                        }
                        eqRunner.process (l.data(), r.data(), n, false);
                        remaining -= n;
                    }
                    ed.stepSpectrumForTest();
                    ed.refreshNow();
                }
            }
            const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out.getChildFile (juce::String (c.name) + ".png"));
            if (os.openedOk()) { os.setPosition (0); os.truncate(); png.writeImageToStream (img, os); }
            std::printf ("%s %dx%d\n", c.name, img.getWidth(), img.getHeight());
        }
    }
    return 0;
}
