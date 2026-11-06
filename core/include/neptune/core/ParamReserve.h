#pragma once
// Réserve d'automation : l'hôte ne voit que `count` « emplacements » (paramètres continus 0..1) ; chacun peut être relié à un réglage au choix du plugin.
//   - Un réglage est relié à au plus un emplacement, et un emplacement vise au plus un réglage : jamais deux courbes sur le même réglage.
//   - Un emplacement occupé ne se réassigne pas sans l'accord de l'appelant (`replace`) ; un emplacement verrouillé ne se réassigne ni ne se libère.
//   - À l'assignation, l'emplacement prend la valeur actuelle du réglage (pas de saut).
//   - Synchro dans les deux sens, à chaque bloc : si l'emplacement a bougé (hôte, automation), le réglage le suit ; sinon, si le réglage a bougé (interface), l'emplacement le suit.
//     Quand les deux ont bougé, l'hôte l'emporte. Les réglages à crans se calent sur leurs crans (l'emplacement garde la valeur envoyée par l'hôte).
//   - L'état : les assignations et les verrous (la valeur des emplacements est dans le jeu de paramètres du plugin).
// Le réglage visé est désigné par (scope, sub, param) : le plugin décide ce que cela veut dire (Trident : chemin + 1 et emplacement d'effet).
// Fil de messages : assign / unassign / lock / état. Fil audio : sync, sans allocation ni verrou.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "neptune/core/Parameter.h"

namespace neptune::core
{
class ParamReserve
{
public:
    static constexpr int kMaxSlots = 256;

    struct Target
    {
        bool active = false;
        std::uint32_t scope = 0;   // 0..65535
        std::uint32_t sub = 0;     // 0..65535
        ParamId param = 0;         // moins de 2^31
        bool operator== (const Target& o) const noexcept { return active == o.active && scope == o.scope && sub == o.sub && param == o.param; }
        bool sameAs (std::uint32_t s, std::uint32_t u, ParamId p) const noexcept { return active && scope == s && sub == u && param == p; }
    };
    // Ce que le plugin répond quand on lui demande où est un réglage visé (set nul : il n'existe plus).
    struct Ref
    {
        ParameterSet* set = nullptr;
        std::size_t index = 0;
    };
    enum class Result { Ok, BadSlot, Locked, SlotTaken, TargetTaken };

    explicit ParamReserve (int count = kMaxSlots) noexcept : count_ (std::clamp (count, 0, kMaxSlots)) {}
    int count() const noexcept { return count_; }

    // ---------------------------------------------------------------- fil de messages
    Target target (int k) const noexcept { return valid (k) ? targets_[static_cast<std::size_t> (k)] : Target {}; }
    bool locked (int k) const noexcept { return valid (k) && locked_[static_cast<std::size_t> (k)]; }
    void setLocked (int k, bool on) noexcept { if (valid (k)) locked_[static_cast<std::size_t> (k)] = on; }
    // Premier emplacement libre (non assigné), ou -1.
    int firstFree() const noexcept
    {
        for (int k = 0; k < count_; ++k)
            if (! targets_[static_cast<std::size_t> (k)].active) return k;
        return -1;
    }
    // Emplacement qui vise ce réglage, ou -1.
    int slotOf (std::uint32_t scope, std::uint32_t sub, ParamId param) const noexcept
    {
        for (int k = 0; k < count_; ++k)
            if (targets_[static_cast<std::size_t> (k)].sameAs (scope, sub, param)) return k;
        return -1;
    }
    int usedCount() const noexcept
    {
        int n = 0;
        for (int k = 0; k < count_; ++k) n += targets_[static_cast<std::size_t> (k)].active ? 1 : 0;
        return n;
    }

    // Relie l'emplacement k à un réglage. `current` : valeur normalisée actuelle du réglage (l'emplacement la prend ; l'appelant la recopie dans le paramètre d'hôte).
    // Refus : emplacement hors limites, verrouillé, déjà occupé (sauf `replace`), réglage déjà relié à un autre emplacement (`TargetTaken` ; `takenBy` dit lequel).
    Result assign (int k, std::uint32_t scope, std::uint32_t sub, ParamId param, double current, bool replace = false, int* takenBy = nullptr) noexcept
    {
        if (! valid (k)) return Result::BadSlot;
        const auto ku = static_cast<std::size_t> (k);
        if (locked_[ku]) return Result::Locked;
        const int other = slotOf (scope, sub, param);
        if (other >= 0 && other != k)
        {
            if (takenBy != nullptr) *takenBy = other;
            return Result::TargetTaken;
        }
        if (targets_[ku].active && ! targets_[ku].sameAs (scope, sub, param) && ! replace) return Result::SlotTaken;
        packed_[ku].store (0, std::memory_order_release);   // le fil audio ne voit pas un état à moitié écrit
        lastSlot_[ku].store (current, std::memory_order_relaxed);
        lastTarget_[ku].store (current, std::memory_order_relaxed);
        followed_[ku].store (false, std::memory_order_relaxed);
        targets_[ku] = { true, scope, sub, param };
        packed_[ku].store (pack (targets_[ku]), std::memory_order_release);
        return Result::Ok;
    }
    // Libère l'emplacement (sans effet s'il est libre). Refusé s'il est verrouillé.
    Result unassign (int k) noexcept
    {
        if (! valid (k)) return Result::BadSlot;
        const auto ku = static_cast<std::size_t> (k);
        if (locked_[ku]) return Result::Locked;
        release (k);
        return Result::Ok;
    }
    // Libère tout, verrous compris (changement de projet, état rappelé).
    void clear() noexcept
    {
        for (int k = 0; k < kMaxSlots; ++k) release (k), locked_[static_cast<std::size_t> (k)] = false;
    }
    // Retire les assignations dont le réglage n'existe plus (`exists(Target)` renvoie faux), verrous compris : un réglage disparu ne se pilote pas.
    template <class F>
    void prune (F&& exists) noexcept
    {
        for (int k = 0; k < count_; ++k)
            if (targets_[static_cast<std::size_t> (k)].active && ! exists (targets_[static_cast<std::size_t> (k)])) release (k);
    }

    nlohmann::json toJson() const
    {
        auto arr = nlohmann::json::array();
        for (int k = 0; k < count_; ++k)
        {
            const auto ku = static_cast<std::size_t> (k);
            const auto& t = targets_[ku];
            if (! t.active && ! locked_[ku]) { arr.push_back (nullptr); continue; }
            nlohmann::json e = nlohmann::json::object();
            if (t.active) { e["scope"] = t.scope; e["sub"] = t.sub; e["param"] = t.param; }
            if (locked_[ku]) e["locked"] = true;
            arr.push_back (e);
        }
        return arr;
    }
    // Relit les assignations ; `exists(Target)` dit si le réglage existe (sinon l'entrée est ignorée). `current(Target)` donne sa valeur normalisée actuelle.
    template <class Exists, class Current>
    void fromJson (const nlohmann::json& arr, Exists&& exists, Current&& current)
    {
        clear();
        if (! arr.is_array()) return;
        for (int k = 0; k < count_ && static_cast<std::size_t> (k) < arr.size(); ++k)
        {
            const auto& e = arr[static_cast<std::size_t> (k)];
            if (! e.is_object()) continue;
            if (e.contains ("scope"))
            {
                Target t { true, e.value ("scope", 0u), e.value ("sub", 0u), static_cast<ParamId> (e.value ("param", 0u)) };
                if (exists (t) && slotOf (t.scope, t.sub, t.param) < 0) assign (k, t.scope, t.sub, t.param, current (t));
            }
            if (e.value ("locked", false)) locked_[static_cast<std::size_t> (k)] = true;
        }
    }

    // ---------------------------------------------------------------- fil audio
    // `slots` : jeu de paramètres qui porte les emplacements (l'emplacement k est à l'indice `first + k`). `resolve(scope, sub, param)` renvoie un Ref.
    template <class Resolve>
    void sync (ParameterSet& slots, std::size_t first, Resolve&& resolve) noexcept
    {
        for (int k = 0; k < count_; ++k)
        {
            const auto ku = static_cast<std::size_t> (k);
            const auto packed = packed_[ku].load (std::memory_order_acquire);
            if (packed == 0) continue;
            const Target t = unpack (packed);
            const Ref ref = resolve (t.scope, t.sub, t.param);
            if (ref.set == nullptr) continue;
            const double sv = slots.getNormalizedAt (first + ku);
            if (sv != lastSlot_[ku].load (std::memory_order_relaxed))
            {
                // l'hôte (ou l'interface de l'emplacement) a bougé : le réglage suit
                ref.set->setNormalizedAt (ref.index, sv);
                lastSlot_[ku].store (sv, std::memory_order_relaxed);
                lastTarget_[ku].store (ref.set->getNormalizedAt (ref.index), std::memory_order_relaxed);
                continue;
            }
            const double tv = ref.set->getNormalizedAt (ref.index);
            if (tv != lastTarget_[ku].load (std::memory_order_relaxed))
            {
                // le réglage a bougé (potard à l'écran) : l'emplacement le suit
                slots.setNormalizedAt (first + ku, tv);
                lastSlot_[ku].store (slots.getNormalizedAt (first + ku), std::memory_order_relaxed);
                lastTarget_[ku].store (tv, std::memory_order_relaxed);
                followed_[ku].store (true, std::memory_order_release);
            }
        }
    }
    // Après un rappel d'état ou de preset : ni l'emplacement ni le réglage ne doivent écraser l'autre. Fil de messages, hors lecture.
    template <class Resolve>
    void resync (ParameterSet& slots, std::size_t first, Resolve&& resolve) noexcept
    {
        for (int k = 0; k < count_; ++k)
        {
            const auto ku = static_cast<std::size_t> (k);
            if (packed_[ku].load (std::memory_order_acquire) == 0) continue;
            lastSlot_[ku].store (slots.getNormalizedAt (first + ku), std::memory_order_relaxed);
            const Target t = targets_[ku];
            const Ref ref = resolve (t.scope, t.sub, t.param);
            if (ref.set != nullptr) lastTarget_[ku].store (ref.set->getNormalizedAt (ref.index), std::memory_order_relaxed);
        }
    }
    // Vrai une fois si l'emplacement a été mis à jour par `sync` à la suite d'un mouvement du réglage : l'appelant en informe l'hôte (écriture d'automation, affichage).
    bool takeFollowed (int k) noexcept { return valid (k) && followed_[static_cast<std::size_t> (k)].exchange (false, std::memory_order_acq_rel); }

private:
    bool valid (int k) const noexcept { return k >= 0 && k < count_; }
    void release (int k) noexcept
    {
        const auto ku = static_cast<std::size_t> (k);
        packed_[ku].store (0, std::memory_order_release);
        targets_[ku] = {};
        followed_[ku].store (false, std::memory_order_relaxed);
    }
    static std::uint64_t pack (const Target& t) noexcept
    {
        return 1ull | (static_cast<std::uint64_t> (t.scope) & 0xFFFFull) << 1 | (static_cast<std::uint64_t> (t.sub) & 0xFFFFull) << 17 | (static_cast<std::uint64_t> (t.param) & 0x7FFFFFFFull) << 33;
    }
    static Target unpack (std::uint64_t v) noexcept
    {
        Target t;
        t.active = (v & 1ull) != 0;
        t.scope = static_cast<std::uint32_t> (v >> 1 & 0xFFFFull);
        t.sub = static_cast<std::uint32_t> (v >> 17 & 0xFFFFull);
        t.param = static_cast<ParamId> (v >> 33 & 0x7FFFFFFFull);
        return t;
    }

    int count_ = kMaxSlots;
    std::array<Target, kMaxSlots> targets_ {};                    // fil de messages
    std::array<bool, kMaxSlots> locked_ {};                       // fil de messages
    std::array<std::atomic<std::uint64_t>, kMaxSlots> packed_ {}; // lu par le fil audio
    std::array<std::atomic<double>, kMaxSlots> lastSlot_ {};      // fil audio
    std::array<std::atomic<double>, kMaxSlots> lastTarget_ {};    // fil audio
    std::array<std::atomic<bool>, kMaxSlots> followed_ {};
};
} // namespace neptune::core
