#pragma once
// Interface du compresseur au style NOVA (maquette design/nova/ui/nova_compressor_full_ui.html) : pages COMPRESSOR (vu-mètres, courbe de transfert
// en direct, six potards à curseurs secondaires), SIDECHAIN (clé, filtres de la clé en cinq zones, réponse en fréquence) et ADVANCED (réglages de ZL
// absents de la maquette : plancher, plage, maintien, liaison et jeux stéréo, RMS, ...). Même code pour le plugin séparé et pour l'insert de Neptune.
#include "NovaEditor.h"
#include "NovaHistoryView.h"
#include "neptune/core/CompressorModule.h"

class NovaCompressorEditor : public nova::EditorBase
{
public:
    NovaCompressorEditor (ParamHost&, const neptune::core::CompressorModule*, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
                          const neptune::core::i18n::Translator&, Options);
    ~NovaCompressorEditor() override;
    void setPageForTest (int p) { setPage (p); }
    void stepHistoryForTest (double nowSeconds) { history_->step (nowSeconds); }

private:
    class SideEqView;
    struct Zone;
    class MidiShapeView;

    void layout() override;
    void tick() override;
    void paintBackground (juce::Graphics&) override;
    std::vector<juce::String> presetNames() const override;
    std::vector<std::pair<neptune::core::ParamId, double>> presetValues (int) const override;
    std::vector<neptune::core::ParamId> presetParamIds() const override;
    void buildCompressorPage();
    void buildSidechainPage();
    void buildAdvancedPage();
    void buildBottomBar();

    const neptune::core::CompressorModule* module_;
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr, *curvePanel_ = nullptr, *scPanel_ = nullptr, *advPanel_ = nullptr;
    nova::VMeter *inMeter_ = nullptr, *grMeter_ = nullptr, *outMeter_ = nullptr;
    nova::CompressorHistoryView* history_ = nullptr;
    nova::Pill* analyzerPill_ = nullptr;
    SideEqView* sideEq_ = nullptr;
    MidiShapeView* shapeView_ = nullptr;
    nova::Pill* channelPill_ = nullptr;
    nova::Text* midiNote_ = nullptr;

    struct Column { nova::Panel* card; nova::KnobCard* knob; nova::SubSlider* sub; };
    std::vector<Column> columns_;
    std::vector<std::unique_ptr<Zone>> zones_;
    struct AdvCard { nova::KnobCard* card; };
    std::vector<nova::KnobCard*> advCards_;
    std::vector<nova::Pill*> advPills_;
    nova::Pill *sourcePill_ = nullptr, *listenPill_ = nullptr, *eqPill_ = nullptr, *rmsPill_ = nullptr;
    nova::SubSlider *keyGain_ = nullptr, *rmsLength_ = nullptr;
    nova::Pill *inTrimPill_ = nullptr, *outTrimPill_ = nullptr;
    nova::Pill *stereoPill_ = nullptr, *stylePill_ = nullptr, *dirPill_ = nullptr, *deltaPill_ = nullptr, *osPill_ = nullptr, *lookPill_ = nullptr;
    juce::String metaText_, latencyText_;
    bool latencyWarn_ = false;
};
