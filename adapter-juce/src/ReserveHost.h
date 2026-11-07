#pragma once
// Réserve d'automation d'un plugin séparé, côté hôte : les emplacements sont les seuls paramètres que l'hôte voit (NeptuneParameter sur un jeu de paramètres propre) ;
// chacun est relié par ParamReserve à un réglage du module. Ce que fait ce composant à la place du plugin :
//   - créer les paramètres d'hôte des emplacements, les renommer d'après leur réglage (« 12 · Seuil »), prévenir l'hôte quand un réglage assigné a bougé à l'écran ;
//   - relier, libérer, verrouiller (les requêtes de ReserveMenu.h / ReserveOverlay.h passent par ici) ;
//   - la synchro audio (sync), l'état (toJson / fromJson).
// Le plugin fournit `resolve(scope, sub, param)` : où se trouve le réglage (jeu de paramètres + indice), et `describe(param)` : son nom pour l'affichage.
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "HostProbe.h"
#include "NeptuneParameter.h"
#include "neptune/core/ParamReserve.h"

class ReserveHost
{
public:
    using Resolve = std::function<neptune::core::ParamReserve::Ref (std::uint32_t, std::uint32_t, neptune::core::ParamId)>;
    using Describe = std::function<std::string (const neptune::core::ParamReserve::Target&)>;

    ReserveHost (int count, Resolve resolve, Describe describe) : reserve_ (count), resolve_ (std::move (resolve)), describe_ (std::move (describe))
    {
        for (int k = 0; k < reserve_.count(); ++k)
            slots_.add (neptune::core::ParamSpec::continuous (kSlotIdBase + static_cast<neptune::core::ParamId> (k), "Auto " + std::to_string (k + 1), neptune::core::Unit::None, 0.0, 1.0, 0.0));
    }

    // Déclare les emplacements à l'hôte (à appeler dans le constructeur du processeur, une fois).
    void addHostParameters (juce::AudioProcessor& processor, HostProbe& probe)
    {
        base_ = processor.getParameters().size();
        for (int k = 0; k < reserve_.count(); ++k) processor.addParameter (new NeptuneParameter (slots_, static_cast<std::size_t> (k), probe));
    }

    int count() const noexcept { return reserve_.count(); }
    neptune::core::ParamReserve& reserve() noexcept { return reserve_; }
    const neptune::core::ParamReserve& reserve() const noexcept { return reserve_; }

    // ---- fil audio : à appeler avant le traitement
    void sync() noexcept { reserve_.sync (slots_, 0, resolve_); }

    // ---- fil de messages
    int slotOf (neptune::core::ParamId id) const noexcept { return reserve_.slotOf (0, 0, id); }
    std::string slotName (int k) const
    {
        const auto t = reserve_.target (k);
        return t.active ? describe_ (t) : std::string();
    }
    bool locked (int k) const noexcept { return reserve_.locked (k); }
    // Le paramètre d'hôte qui porte le réglage (pour les gestes d'édition), ou nullptr s'il n'est pas assigné.
    juce::AudioProcessorParameter* hostParameterFor (juce::AudioProcessor& p, neptune::core::ParamId id) const
    {
        const int k = slotOf (id);
        return k >= 0 ? p.getParameters()[base_ + k] : nullptr;
    }
    // Relie le réglage à `slot` (-1 : le premier libre). Renvoie l'emplacement utilisé, ou -1.
    int assign (juce::AudioProcessor& p, neptune::core::ParamId id, int slot, bool replace)
    {
        if (slot < 0) slot = reserve_.firstFree();
        if (slot < 0) return -1;
        const auto ref = resolve_ (0, 0, id);
        if (ref.set == nullptr) return -1;
        const double n = ref.set->getNormalizedAt (ref.index);
        if (reserve_.assign (slot, 0, 0, id, n, replace) != neptune::core::ParamReserve::Result::Ok) return -1;
        slots_.setNormalizedAt (static_cast<std::size_t> (slot), n);
        refreshNames (p);
        return slot;
    }
    bool release (juce::AudioProcessor& p, int slot)
    {
        if (reserve_.unassign (slot) != neptune::core::ParamReserve::Result::Ok) return false;
        refreshNames (p);
        return true;
    }
    void setLocked (juce::AudioProcessor& p, int slot, bool on)
    {
        reserve_.setLocked (slot, on);
        refreshNames (p);
    }
    // Les noms que l'hôte affiche (« 3 · Band 2 Freq ») ; à rappeler après tout changement d'assignation.
    void refreshNames (juce::AudioProcessor& p)
    {
        for (int k = 0; k < reserve_.count(); ++k)
            if (auto* np = dynamic_cast<NeptuneParameter*> (p.getParameters()[base_ + k]))
            {
                const auto name = slotName (k);
                np->setDisplayName (name.empty() ? juce::String() : juce::String (k + 1) + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String::fromUTF8 (name.c_str()));
            }
        p.updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged (true));
    }
    // Minuterie : les réglages assignés qui ont bougé sans passer par l'interface (preset, A/B) font suivre leur emplacement ; l'hôte doit en être informé.
    void notifyFollowed (juce::AudioProcessor& p)
    {
        for (int k = 0; k < reserve_.count(); ++k)
            if (reserve_.takeFollowed (k))
                if (auto* hp = p.getParameters()[base_ + k]) hp->setValueNotifyingHost (hp->getValue());
    }

    // ---- état
    nlohmann::json toJson() const { return reserve_.toJson(); }
    // Les valeurs des emplacements : ce sont des paramètres que l'hôte lit et écrit, ils doivent revenir tels quels avec l'état (clap-validator : state-reproducibility).
    nlohmann::json valuesToJson() const
    {
        auto arr = nlohmann::json::array();
        for (int k = 0; k < reserve_.count(); ++k) arr.push_back (slots_.getNormalizedAt (static_cast<std::size_t> (k)));
        return arr;
    }
    void fromJson (juce::AudioProcessor& p, const nlohmann::json& arr, const nlohmann::json& values = nlohmann::json::array())
    {
        reserve_.fromJson (arr,
                           [this] (const neptune::core::ParamReserve::Target& t) { return resolve_ (t.scope, t.sub, t.param).set != nullptr; },
                           [this] (const neptune::core::ParamReserve::Target& t) { const auto r = resolve_ (t.scope, t.sub, t.param); return r.set->getNormalizedAt (r.index); });
        for (int k = 0; k < reserve_.count(); ++k)
            if (const auto t = reserve_.target (k); t.active)
            {
                const auto r = resolve_ (t.scope, t.sub, t.param);
                slots_.setNormalizedAt (static_cast<std::size_t> (k), r.set->getNormalizedAt (r.index));
            }
        // un emplacement libre garde la valeur enregistrée ; un emplacement assigné prend celle de son réglage (au-dessus)
        if (values.is_array())
            for (int k = 0; k < reserve_.count() && static_cast<std::size_t> (k) < values.size(); ++k)
                if (! reserve_.target (k).active && values[static_cast<std::size_t> (k)].is_number())
                    slots_.setNormalizedAt (static_cast<std::size_t> (k), std::clamp (values[static_cast<std::size_t> (k)].get<double>(), 0.0, 1.0));
        reserve_.resync (slots_, 0, resolve_);
        refreshNames (p);
    }

private:
    static constexpr neptune::core::ParamId kSlotIdBase = 100000;
    neptune::core::ParameterSet slots_;
    neptune::core::ParamReserve reserve_;
    Resolve resolve_;
    Describe describe_;
    int base_ = 0;
};
