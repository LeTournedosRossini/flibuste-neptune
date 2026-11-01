#pragma once
// Interface de Digue (limiteur brickwall) au style NOVA, sur le socle commun nova::EditorBase : une page DIGUE (mètres d'entrée, de réduction de gain et de sortie,
// historique de la réduction de gain, cinq réglages, bouton High Quality) et la page SETTINGS. Plan : docs/plan-limiteur.md.
#include "NovaEditor.h"
#include "neptune/core/LimiterModule.h"

class NovaDigueEditor : public nova::EditorBase
{
public:
    NovaDigueEditor (ParamHost&, const neptune::core::LimiterModule*, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
                     const neptune::core::i18n::Translator&, Options);
    ~NovaDigueEditor() override;

private:
    class HistoryView;

    void layout() override;
    void tick() override;
    bool isHighQuality() const;
    void applyHighQuality();

    const neptune::core::LimiterModule* module_;
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr, *viewPanel_ = nullptr;
    nova::VMeter *inMeter_ = nullptr, *grMeter_ = nullptr, *outMeter_ = nullptr;
    nova::Pill *inTrimPill_ = nullptr, *modePill_ = nullptr, *hqPill_ = nullptr;
    nova::Text *latencyText_ = nullptr, *latencyWarn_ = nullptr;
    HistoryView* view_ = nullptr;
    struct Column { nova::Panel* card; nova::KnobCard* knob; };
    std::vector<Column> columns_;   // THRESHOLD, CEILING, RELEASE, LOOKAHEAD, OUTPUT
};
