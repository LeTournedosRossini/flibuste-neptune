#pragma once
// Interface de Fournaise (overdrive à quatre modèles) au style NOVA, sur le socle commun nova::EditorBase : page SATURATION (modèles, courbe de transfert, drive, polarisation,
// caractère, sensation, tonalité, niveau, entrée / mélange / sortie), page FILTRES (entrée et sortie), page EGALISEUR (trois bandes, passe-haut, passe-bas), page ENTREE LATERALE
// (la clé module le drive) et page SETTINGS. La fenêtre est étirable en largeur (stretchAuto).
#include <array>
#include <vector>

#include "NovaEditor.h"
#include "neptune/core/OverdriveModule.h"

class NovaFournaiseEditor : public nova::EditorBase
{
public:
    NovaFournaiseEditor (ParamHost&, const neptune::core::OverdriveModule*, const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
                         const neptune::core::i18n::Translator&, Options);
    ~NovaFournaiseEditor() override;
    void setPageForTest (int p) { setPage (p); }

private:
    class ChipRow;
    class TransferView;
    class EqView;
    class KeyView;
    struct EqColumn { nova::Pill *on = nullptr, *type = nullptr, *slope = nullptr; };
    struct FilterBlock { nova::Pill* on = nullptr; nova::KnobCard* freq = nullptr; nova::Pill* slope = nullptr; };

    void layout() override;
    void tick() override;
    void buildCommon();
    void buildSaturationPage();
    void buildFiltersPage();
    void buildEqPage();
    void buildKeyPage();
    void retitleKnobs();   // les titres de « Bias » et « Feel » suivent le modèle
    std::vector<juce::String> presetNames() const override;
    std::vector<std::pair<neptune::core::ParamId, double>> presetValues (int index) const override;
    std::vector<neptune::core::ParamId> presetParamIds() const override;

    const neptune::core::OverdriveModule* module_;

    // en commun (toutes les pages)
    nova::Panel *metersPanel_ = nullptr, *navPanel_ = nullptr;
    nova::VMeter *inMeter_ = nullptr, *outMeter_ = nullptr;
    nova::Pill* resetPill_ = nullptr;

    // page SATURATION
    nova::Panel* satPanel_ = nullptr;
    ChipRow* modelChips_ = nullptr;
    TransferView* transfer_ = nullptr;
    nova::KnobCard *driveKnob_ = nullptr, *biasKnob_ = nullptr, *charKnob_ = nullptr, *feelKnob_ = nullptr, *toneKnob_ = nullptr, *levelKnob_ = nullptr;
    nova::KnobCard *trimInKnob_ = nullptr, *mixKnob_ = nullptr, *trimOutKnob_ = nullptr, *smoothKnob_ = nullptr, *tightKnob_ = nullptr, *imperfectionKnob_ = nullptr;
    nova::Pill *autoPill_ = nullptr, *osPill_ = nullptr, *chPill_ = nullptr;
    int shownModel_ = -1;

    // page FILTRES : passe-haut et passe-bas d'entrée, puis de sortie
    nova::Panel *inFiltersPanel_ = nullptr, *outFiltersPanel_ = nullptr;
    std::array<FilterBlock, 4> filters_ {};

    // page EGALISEUR
    nova::Panel* eqPanel_ = nullptr;
    nova::Pill* eqPill_ = nullptr;
    EqView* eqView_ = nullptr;
    std::vector<EqColumn> eqColumns_;

    // page ENTREE LATERALE
    nova::Panel* keyPanel_ = nullptr;
    nova::Pill* keySourcePill_ = nullptr;
    std::vector<nova::KnobCard*> keyKnobs_;
    KeyView* keyView_ = nullptr;
    nova::Text* keyHint_ = nullptr;
    nova::Text* osNote_ = nullptr;
};
