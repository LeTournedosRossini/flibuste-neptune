#pragma once
// Interface de La Flèche (shaper de transitoires) au style NOVA (maquette design/nova/ui/nova_transient_shaper_ui.html), sur le même socle que le
// compresseur et l'égaliseur (nova::EditorBase) : pages SHAPER (vu-mètres, graphe WAVE ou RESPONSE, six potards, jeu 2 des gains), SIDECHAIN (source, clé,
// égaliseur de détection en cinq zones avec analyseur), ADVANCED (détection) et SETTINGS (thème, contraste, langue, presets utilisateur).
// Même code pour le plugin séparé et pour l'insert de Neptune.
#include "NovaEditor.h"
#include "NovaShaperWaveView.h"
#include "neptune/core/ShaperModule.h"

class NovaShaperEditor : public nova::EditorBase
{
public:
    NovaShaperEditor (ParamHost&, const neptune::core::ShaperModule*, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
                      const neptune::core::i18n::Translator&, Options);
    ~NovaShaperEditor() override;
    void setPageForTest (int p) { setPage (p); }
    void showResponseForTest (bool r) { showResponse_ = r; refreshNow(); }
    void stepWaveForTest (double nowSeconds) { wave_->step (nowSeconds); }

private:
    class ResponseView;
    class EqView;
    struct Zone;

    void layout() override;
    void tick() override;
    void paintBackground (juce::Graphics&) override;
    std::vector<juce::String> presetNames() const override;
    std::vector<std::pair<neptune::core::ParamId, double>> presetValues (int) const override;
    std::vector<neptune::core::ParamId> presetParamIds() const override;
    void buildShaperPage();
    void buildSidechainPage();
    void buildAdvancedPage();
    void buildBottomBar();

    const neptune::core::ShaperModule* module_;
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr, *detPanel_ = nullptr, *scPanel_ = nullptr, *advPanel_ = nullptr;
    nova::VMeter *inMeter_ = nullptr, *outMeter_ = nullptr;
    nova::Pill *inTrimPill_ = nullptr, *viewPill_ = nullptr, *gearPill_ = nullptr;
    nova::ShaperWaveView* wave_ = nullptr;
    ResponseView* response_ = nullptr;
    bool showResponse_ = false;

    struct Column { nova::Panel* card; nova::KnobCard* knob; nova::SubSlider* sub; };
    std::vector<Column> columns_;
    std::vector<nova::SubSlider*> bars_;   // rangée de curseurs sous le graphe

    // page SIDECHAIN
    nova::Pill *enablePill_ = nullptr, *sourcePill_ = nullptr, *listenPill_ = nullptr, *eqPill_ = nullptr, *spectrumPill_ = nullptr, *extMsPill_ = nullptr;
    nova::Pill *phaseIntPill_ = nullptr, *phaseExtPill_ = nullptr;
    nova::SubSlider *gainSlider_ = nullptr, *intVolSlider_ = nullptr, *extVolSlider_ = nullptr;
    nova::Text* scNote_ = nullptr;
    EqView* eq_ = nullptr;
    std::vector<std::unique_ptr<Zone>> zones_;

    // page ADVANCED
    nova::Pill* detPill_ = nullptr;
    std::vector<nova::KnobCard*> advCards_;

    nova::Pill *chPill_ = nullptr, *deltaPill_ = nullptr, *osPill_ = nullptr, *lookPill_ = nullptr;
    juce::String metaText_, latencyText_;
    bool latencyWarn_ = false;
};
