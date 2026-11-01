#pragma once
// Module « stock » (spec-modules §7) : un effet audio stéréo dont le DSP et la description des paramètres vivent ici, dans le
// cœur portable, indépendamment de l'hôte. Deux adaptateurs minces l'utilisent : un emplacement d'insert de Neptune (Engine) et
// un plugin CLAP séparé (adapter-juce). L'interface JUCE d'un module ne connaît qu'une source de paramètres abstraite.
//
//  - StockModule : le DSP d'un effet (ses paramètres propres : identifiants >= stock::firstModuleParam).
//  - ModuleRunner : ce que tous les modules ont en commun (spec §7) : trim d'entrée, trim de sortie, mix dry/wet, bypass,
//                   avec lissage. Possède l'ensemble de paramètres du module (communs + propres).
//  - InsertInstance : un module prêt à être posé sur un emplacement (type + runner).
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "neptune/core/Parameter.h"

namespace neptune::core
{
namespace stock
{
// Paramètres communs à tous les modules (identifiants stables).
constexpr ParamId inputTrim = 1;   // dB, -24..+24
constexpr ParamId outputTrim = 2;  // dB, -24..+24
constexpr ParamId mix = 3;         // %, 0..100 (dry/wet)
constexpr ParamId bypass = 4;      // bascule
constexpr ParamId firstModuleParam = 100;
} // namespace stock

class StockModule
{
public:
    virtual ~StockModule() = default;

    virtual std::string_view typeId() const noexcept = 0; // "utility"
    // Déclare les paramètres PROPRES au module (identifiants >= stock::firstModuleParam). Hors temps réel.
    virtual bool declareParams (ParameterSet&, std::string* error) const = 0;
    // Résout les identifiants en index une fois (chemin chaud sans recherche).
    virtual void bind (const ParameterSet&) = 0;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() noexcept = 0;
    // Traite `n` échantillons en place. Aucune allocation.
    virtual void process (float* left, float* right, int n, const ParameterSet& params) noexcept = 0;
    // Latence déclarée à l'hôte (échantillons) ; 0 si aucune. Elle peut dépendre de réglages non automatisables (lookahead, oversampling).
    virtual int latencySamples (const ParameterSet&) const noexcept { return 0; }
    // Borne supérieure de la latence à la cadence donnée (dimensionne le retard du signal sec du ModuleRunner).
    virtual int maxLatencySamples (double /*sampleRate*/) const noexcept { return 0; }
    // Profil Live de Neptune : le module passe en variante à latence nulle (anticipation et suréchantillonnage coupés) SANS toucher à ses réglages ; le retour
    // en profil Studio rend les réglages de l'utilisateur. Appelé par le moteur à chaque bloc (atomique, sans coût).
    virtual void setZeroLatency (bool) noexcept {}
    // Valeur par défaut du mix commun (%) : 100 pour un effet en insert ordinaire, moins pour un effet de départ (delay).
    virtual double defaultMixPercent() const noexcept { return 100.0; }

    // Entrée de clé externe (sidechain), pour les modules qui en ont une : appelée avant process(), valable pour ce bloc seulement ;
    // nuls : pas de clé branchée.
    virtual bool wantsKey() const noexcept { return false; }
    virtual void setKey (const float* /*left*/, const float* /*right*/) noexcept {}

    // Valeurs d'affichage (vu-mètres, corrélateur...), lisibles depuis l'interface sans verrou.
    virtual int numMeters() const noexcept { return 0; }
    virtual float meter (int) const noexcept { return 0.0f; }

    // État « texte » du module, hors paramètres numériques (par exemple le fichier d'une réponse impulsionnelle). Sauvegardé avec l'insert ou le projet.
    virtual bool hasStateString() const noexcept { return false; }
    virtual std::string stateString() const { return {}; }
    virtual void setStateString (const std::string&) {}
};

class ModuleRunner
{
public:
    // Déclare les paramètres communs puis ceux du module. `ok()` rend faux si une déclaration a échoué.
    explicit ModuleRunner (std::unique_ptr<StockModule> module);

    bool ok() const noexcept { return ok_; }
    ParameterSet& params() noexcept { return params_; }
    const ParameterSet& params() const noexcept { return params_; }
    StockModule& module() noexcept { return *module_; }
    const StockModule& module() const noexcept { return *module_; }

    void prepare (double sampleRate, int maxBlockSize);
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlock_; }
    void reset() noexcept;

    // Traite en place. `bypassed` : l'hôte décide (Neptune : paramètre de l'emplacement ; CLAP séparé : paramètre `stock::bypass`).
    void process (float* left, float* right, int n, bool bypassed) noexcept;
    // Idem, le bypass est lu dans le paramètre `stock::bypass` du module (plugin séparé).
    void process (float* left, float* right, int n) noexcept { process (left, right, n, params_.getAt (idxBypass_) >= 0.5); }
    // Clé externe de ce bloc (n échantillons, ou nuls) : transmise au module juste avant son traitement.
    void setKey (const float* left, const float* right) noexcept { keyL_ = left; keyR_ = right; }
    bool wantsKey() const noexcept { return module_->wantsKey(); }

    int latencySamples() const noexcept { return module_->latencySamples (params_); }
    void setZeroLatency (bool zero) noexcept { module_->setZeroLatency (zero); }

private:
    // Les paramètres sont déclarés AVANT le module : à la destruction, le module (et un éventuel thread qui lit les paramètres) disparaît le premier.
    ParameterSet params_;
    // Même déclaration que params_ : quand la matrice de modulation écrit des décalages dans params_, process() recopie ici les valeurs effectives
    // (réglage + décalage) et le module lit cette copie. L'interface, l'hôte et les presets ne voient que params_.
    ParameterSet effective_;
    std::unique_ptr<StockModule> module_;
    bool ok_ = true;
    std::size_t idxIn_ = ParameterSet::npos, idxOut_ = ParameterSet::npos, idxMix_ = ParameterSet::npos, idxBypass_ = ParameterSet::npos;
    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    const ParameterSet& valuesForBlock() noexcept;   // params_, ou la copie effective_ si une modulation est en cours
    std::vector<float> dryL_, dryR_;
    std::vector<float> ringL_, ringR_;     // retard du signal sec : le module a une latence, le sec doit lui être aligné (mix, bypass)
    std::size_t ringPos_ = 0;
    int ringLatency_ = 0;
    static constexpr int kDryFade = 1024;   // fondu enchaîné du sec quand la latence du module change (échantillons)
    int fadeFrom_ = 0, fadeLeft_ = 0;
    const float *keyL_ = nullptr, *keyR_ = nullptr;
    float lastIn_ = 1.0f, lastOut_ = 1.0f, lastMix_ = 1.0f;
    bool idle_ = false; // contourné depuis un moment : le module n'est plus traité
};

// Un module posé sur un emplacement d'insert. Créé par le thread de message, remis au thread audio (HandoffSlot) qui en est
// ensuite le seul utilisateur du DSP ; les paramètres (atomiques) restent modifiables depuis l'interface.
struct InsertInstance
{
    std::string type;
    ModuleRunner runner;
    InsertInstance (std::string t, std::unique_ptr<StockModule> m) : type (std::move (t)), runner (std::move (m)) {}
};

// ---- registre des modules disponibles ----
struct ModuleInfo
{
    std::string_view typeId; // "utility"
    std::string_view nameKey; // clé i18n du nom affiché ("module.utility")
};
const std::vector<ModuleInfo>& availableModules();
std::unique_ptr<StockModule> createModule (std::string_view typeId); // nul si inconnu
// Crée un module posé sur un emplacement, préparé (sampleRate <= 0 : pas encore préparé). Nul si le type est inconnu.
std::shared_ptr<InsertInstance> createInsert (std::string_view typeId, double sampleRate, int maxBlockSize);
} // namespace neptune::core
