#pragma once
// « Diapason » comme module d'insert : le générateur de signaux de test et la boîte à rythme (SignalGenerator.h) posés sur un emplacement de Neptune.
// Le signal produit REMPLACE celui de l'emplacement (le mélange commun, 100 % par défaut, permet de le doser ; le bypass laisse passer le signal d'entrée).
// Un insert ne reçoit ni notes MIDI ni position de lecture : le mode « note MIDI » ne sonne pas et le séquenceur reste sur son tempo interne.
// Les réglages vivent dans le jeu de paramètres du module (interface, état, presets) ; le moteur de génération a son propre jeu, déclaré à l'identique,
// recopié à chaque bloc.
#include <vector>

#include "neptune/core/SignalGenerator.h"
#include "neptune/core/StockModule.h"

namespace neptune::core
{
class DiapasonModule final : public StockModule
{
public:
    DiapasonModule();

    std::string_view typeId() const noexcept override { return "diapason"; }
    bool declareParams (ParameterSet&, std::string* error) const override;
    void bind (const ParameterSet&) override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void reset() noexcept override;
    void process (float* left, float* right, int n, const ParameterSet& params) noexcept override;
    bool isSource() const noexcept override { return true; }

    // L'interface (déclencheur, sons de batterie, affichage du pas et des crêtes) parle directement au moteur.
    generator::GeneratorEngine& engine() noexcept { return engine_; }

private:
    generator::GeneratorEngine engine_;
    std::vector<std::pair<std::size_t, std::size_t>> map_;   // (index dans le jeu du module, index dans le jeu du moteur)
};
} // namespace neptune::core
