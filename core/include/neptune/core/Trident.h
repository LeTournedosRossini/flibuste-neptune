#pragma once
// Trident : un séparateur à trois chemins qui héberge les effets de la suite (modules stock), sans hôte CLAP.
//   - Un séparateur de fréquences (Crossover.h : 2 ou 3 bandes) donne grave, médium, aigu ; chaque chemin choisit sa source : signal complet, grave, médium ou aigu.
//     Parallèle pur : complet / complet / complet. Deux bandes plus un complet : grave / aigu / complet. Trois bandes : grave / médium / aigu.
//   - Chaque chemin a quatre emplacements d'effets (InsertInstance, comme les inserts de Neptune), un volume, un mute, un solo et une inversion de polarité.
//   - Les chemins n'ont pas la même latence (convolution, limiteur...) : chacun est retardé pour que tout soit aligné à la somme ; la latence totale est annoncée à l'hôte.
//   - Banque de 32 paramètres assignables : chacun vise un réglage au choix (d'un effet ou de Trident) ; l'automation de l'hôte passe par eux.
// Fil de messages : effets, assignations, état. Fil audio : `process`, rien d'autre ; aucune allocation ni verrou.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "neptune/core/Crossover.h"
#include "neptune/core/HandoffSlot.h"
#include "neptune/core/ParamReserve.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/StockModule.h"

namespace neptune::core::trident
{
constexpr int kPaths = 3;
constexpr int kSlotsPerPath = 4;
constexpr int kAssignables = 32;

enum class Source : int { Full = 0, Low, Mid, High };

// Identifiants stables des paramètres de Trident.
namespace pid
{
constexpr ParamId bands = 1;        // 2 ou 3 bandes
constexpr ParamId xover1 = 2;       // Hz
constexpr ParamId xover2 = 3;       // Hz
constexpr ParamId slope = 4;        // 12, 24 ou 48 dB/octave
constexpr ParamId inputGain = 5;    // dB
constexpr ParamId outputGain = 6;   // dB
constexpr ParamId pathBase = 10;    // chemin p : pathBase + 10 p + {source, gain, mute, solo, invert}
constexpr ParamId source = 0, gain = 1, mute = 2, solo = 3, invert = 4;
constexpr ParamId assignBase = 100; // assignable k : assignBase + k
constexpr ParamId path (int p, ParamId which) { return pathBase + 10u * static_cast<ParamId> (p) + which; }
constexpr ParamId assign (int k) { return assignBase + static_cast<ParamId> (k); }
} // namespace pid

class Trident
{
public:
    Trident()
    {
        std::string err;
        auto add = [&] (ParamSpec s) { ok_ = params_.add (std::move (s), &err) && ok_; };
        add (ParamSpec::choice (pid::bands, "Bandes", { "2 bandes", "3 bandes" }, 1));
        add (ParamSpec::continuous (pid::xover1, "Coupure 1", Unit::Hertz, 20.0, 20000.0, 250.0, Curve::Exponential));
        add (ParamSpec::continuous (pid::xover2, "Coupure 2", Unit::Hertz, 20.0, 20000.0, 3000.0, Curve::Exponential));
        add (ParamSpec::choice (pid::slope, "Pente", { "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 1));
        add (ParamSpec::continuous (pid::inputGain, "Entrée", Unit::Decibel, -24.0, 24.0, 0.0));
        add (ParamSpec::continuous (pid::outputGain, "Sortie", Unit::Decibel, -60.0, 12.0, 0.0));
        static const char* const pathNames[kPaths] = { "Chemin 1", "Chemin 2", "Chemin 3" };
        for (int p = 0; p < kPaths; ++p)
        {
            const std::string n = pathNames[p];
            add (ParamSpec::choice (pid::path (p, pid::source), n + " source", { "Complet", "Grave", "Médium", "Aigu" }, p + 1));
            add (ParamSpec::continuous (pid::path (p, pid::gain), n + " volume", Unit::Decibel, -60.0, 12.0, 0.0));
            add (ParamSpec::toggle (pid::path (p, pid::mute), n + " mute"));
            add (ParamSpec::toggle (pid::path (p, pid::solo), n + " solo"));
            add (ParamSpec::toggle (pid::path (p, pid::invert), n + " polarité"));
        }
        for (int k = 0; k < kAssignables; ++k)
        {
            auto s = ParamSpec::continuous (pid::assign (k), "Macro " + std::to_string (k + 1), Unit::None, 0.0, 1.0, 0.0);
            s.decimals = 3;
            add (std::move (s));
        }
        for (int k = 0; k < kAssignables; ++k) assignIdx_[static_cast<std::size_t> (k)] = params_.indexOf (pid::assign (k));
        idxBands_ = params_.indexOf (pid::bands);
        idxX1_ = params_.indexOf (pid::xover1);
        idxX2_ = params_.indexOf (pid::xover2);
        idxSlope_ = params_.indexOf (pid::slope);
        idxIn_ = params_.indexOf (pid::inputGain);
        idxOut_ = params_.indexOf (pid::outputGain);
        for (int p = 0; p < kPaths; ++p)
            for (ParamId w = 0; w < 5; ++w) idxPath_[static_cast<std::size_t> (p)][w] = params_.indexOf (pid::path (p, w));
    }

    bool ok() const noexcept { return ok_; }
    ParameterSet& params() noexcept { return params_; }
    const ParameterSet& params() const noexcept { return params_; }

    // ---------------------------------------------------------------- préparation
    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = std::max (1, maxBlockSize);
        crossover_.prepare (sampleRate_);
        const std::size_t cap = static_cast<std::size_t> (maxBlock_);
        for (auto* v : { &inL_, &inR_, &lowL_, &lowR_, &midL_, &midR_, &highL_, &highR_, &fullL_, &fullR_, &outL_, &outR_ }) v->assign (cap, 0.0f);
        for (auto& p : paths_)
        {
            p.bufL.assign (cap, 0.0f);
            p.bufR.assign (cap, 0.0f);
            p.delay.prepare (static_cast<std::size_t> (2.0 * sampleRate_) + cap);
        }
        for (int p = 0; p < kPaths; ++p)
            for (int s = 0; s < kSlotsPerPath; ++s)
                if (msg_[static_cast<std::size_t> (p)][static_cast<std::size_t> (s)] != nullptr) msg_[static_cast<std::size_t> (p)][static_cast<std::size_t> (s)]->runner.prepare (sampleRate_, maxBlock_);
        lastIn_ = lastOut_ = -1.0f;
    }
    void reset() noexcept
    {
        crossover_.reset();
        for (auto& p : paths_) p.delay.clear();
        for (int p = 0; p < kPaths; ++p)
            for (int s = 0; s < kSlotsPerPath; ++s)
                if (auto* inst = const_cast<InsertInstance*> (slots_[static_cast<std::size_t> (p)][static_cast<std::size_t> (s)].current())) inst->runner.reset();
    }
    double sampleRate() const noexcept { return sampleRate_; }

    // ---------------------------------------------------------------- effets (fil de messages)
    // Pose un effet (typeId de `availableModules()`) ; chaîne vide : vide l'emplacement. Faux si le type est inconnu.
    bool setEffect (int path, int slot, const std::string& type)
    {
        if (! valid (path, slot)) return false;
        std::shared_ptr<InsertInstance> inst;
        if (! type.empty())
        {
            inst = createInsert (type, sampleRate_, maxBlock_);
            if (inst == nullptr) return false;
        }
        publish (path, slot, std::move (inst));
        return true;
    }
    // Pose un effet déjà construit (état rappelé).
    void setEffectInstance (int path, int slot, std::shared_ptr<InsertInstance> inst)
    {
        if (valid (path, slot)) publish (path, slot, std::move (inst));
    }
    // L'effet d'un emplacement tel que le fil de messages le voit (nul si vide) : ses paramètres sont atomiques, l'interface peut les lire et les régler.
    std::shared_ptr<InsertInstance> effect (int path, int slot) const
    {
        return valid (path, slot) ? msg_[static_cast<std::size_t> (path)][static_cast<std::size_t> (slot)] : nullptr;
    }
    void collect()
    {
        for (auto& row : slots_)
            for (auto& s : row) s.collect();
    }

    // ---------------------------------------------------------------- paramètres assignables (fil de messages)
    struct Target
    {
        bool active = false;
        int path = -1;        // -1 : un réglage de Trident
        int slot = 0;
        ParamId param = 0;
        bool operator== (const Target& o) const noexcept { return active == o.active && path == o.path && slot == o.slot && param == o.param; }
    };
    Target assignment (int k) const noexcept
    {
        const auto t = reserve_.target (k);
        return t.active ? Target { true, static_cast<int> (t.scope) - 1, static_cast<int> (t.sub), t.param } : Target {};
    }
    const ParamReserve& reserve() const noexcept { return reserve_; }
    ParamReserve& reserve() noexcept { return reserve_; }
    // Vise un réglage ; la valeur de l'assignable prend celle du réglage (pas de saut). Refus (sans rien changer) : cible absente, emplacement verrouillé, ou occupé
    // sans `replace`, ou réglage déjà sur un autre emplacement (`takenBy` dit lequel).
    ParamReserve::Result assignChecked (int k, int path, int slot, ParamId param, bool replace = false, int* takenBy = nullptr)
    {
        if (k < 0 || k >= kAssignables) return ParamReserve::Result::BadSlot;
        const ParameterSet* set = targetSet (path, slot);
        if (set == nullptr) return ParamReserve::Result::BadSlot;
        const auto idx = set->indexOf (param);
        if (idx == ParameterSet::npos) return ParamReserve::Result::BadSlot;
        const double n = set->getNormalizedAt (idx);
        const auto r = reserve_.assign (k, static_cast<std::uint32_t> (path + 1), static_cast<std::uint32_t> (slot), param, n, replace, takenBy);
        if (r == ParamReserve::Result::Ok) params_.setNormalizedAt (assignIdx_[static_cast<std::size_t> (k)], n);
        return r;
    }
    // Comme avant la réserve : l'assignation prend la place (le réglage quitte son ancien emplacement s'il n'est pas verrouillé). Faux si refusée.
    bool assign (int k, int path, int slot, ParamId param)
    {
        int taken = -1;
        auto r = assignChecked (k, path, slot, param, true, &taken);
        if (r == ParamReserve::Result::TargetTaken && taken >= 0 && reserve_.unassign (taken) == ParamReserve::Result::Ok)
            r = assignChecked (k, path, slot, param, true);
        return r == ParamReserve::Result::Ok;
    }
    bool unassign (int k) { return reserve_.unassign (k) == ParamReserve::Result::Ok; }
    // Après un rappel d'état : les assignables ne doivent pas écraser les réglages rappelés.
    void syncAssignments() noexcept
    {
        reserve_.resync (params_, assignIdx_[0], [this] (std::uint32_t scope, std::uint32_t sub, ParamId param) { return resolveMsg (scope, sub, param); });
    }
    // Les emplacements dont le réglage a bougé à l'écran : vrai une fois (pour prévenir l'hôte). Fil de messages.
    bool takeFollowed (int k) noexcept { return reserve_.takeFollowed (k); }

    // ---------------------------------------------------------------- état
    // L'état complet : paramètres de Trident, assignations, effets (type, valeurs, état texte).
    std::string stateToJson() const
    {
        nlohmann::json doc;
        doc["format"] = "trident-state";
        doc["version"] = 1;
        nlohmann::json own = nlohmann::json::object();
        for (std::size_t i = 0; i < params_.size(); ++i) own[std::to_string (params_.spec (i).id)] = params_.getAt (i);
        doc["params"] = own;
        doc["assign"] = reserve_.toJson();
        auto fx = nlohmann::json::array();
        for (int p = 0; p < kPaths; ++p)
            for (int s = 0; s < kSlotsPerPath; ++s)
            {
                const auto& inst = msg_[static_cast<std::size_t> (p)][static_cast<std::size_t> (s)];
                if (inst == nullptr) { fx.push_back (nullptr); continue; }
                nlohmann::json e = { { "type", inst->type } };
                nlohmann::json vals = nlohmann::json::object();
                const auto& set = inst->runner.params();
                for (std::size_t i = 0; i < set.size(); ++i) vals[std::to_string (set.spec (i).id)] = set.getAt (i);
                e["values"] = vals;
                if (inst->runner.module().hasStateString()) e["text"] = inst->runner.module().stateString();
                fx.push_back (e);
            }
        doc["effects"] = fx;
        return doc.dump();
    }
    bool stateFromJson (const std::string& text)
    {
        const auto doc = nlohmann::json::parse (text, nullptr, false);
        if (! doc.is_object() || doc.value ("format", std::string()) != "trident-state") return false;
        if (doc.contains ("params") && doc["params"].is_object())
            for (const auto& [key, v] : doc["params"].items())
                if (v.is_number()) params_.set (static_cast<ParamId> (std::strtoul (key.c_str(), nullptr, 10)), v.get<double>());
        if (doc.contains ("effects") && doc["effects"].is_array())
            for (int p = 0; p < kPaths; ++p)
                for (int s = 0; s < kSlotsPerPath; ++s)
                {
                    const std::size_t i = static_cast<std::size_t> (p * kSlotsPerPath + s);
                    std::shared_ptr<InsertInstance> inst;
                    if (i < doc["effects"].size() && doc["effects"][i].is_object())
                    {
                        const auto& e = doc["effects"][i];
                        inst = createInsert (e.value ("type", std::string()), sampleRate_, maxBlock_);
                        if (inst != nullptr)
                        {
                            if (e.contains ("values") && e["values"].is_object())
                                for (const auto& [key, v] : e["values"].items())
                                    if (v.is_number()) inst->runner.params().set (static_cast<ParamId> (std::strtoul (key.c_str(), nullptr, 10)), v.get<double>());
                            if (e.contains ("text") && e["text"].is_string()) inst->runner.module().setStateString (e["text"].get<std::string>());
                        }
                    }
                    publish (p, s, std::move (inst));
                }
        // assignations : le format d'avant la réserve donnait { path, slot, param } ; on le convertit
        nlohmann::json assigns = doc.contains ("assign") ? doc["assign"] : nlohmann::json::array();
        if (assigns.is_array())
            for (auto& e : assigns)
                if (e.is_object() && e.contains ("path") && ! e.contains ("scope"))
                {
                    e["scope"] = e.value ("path", -1) + 1;
                    e["sub"] = e.value ("slot", 0);
                }
        reserve_.fromJson (assigns,
                           [this] (const ParamReserve::Target& t) { const auto* set = targetSet (static_cast<int> (t.scope) - 1, static_cast<int> (t.sub)); return set != nullptr && set->indexOf (t.param) != ParameterSet::npos; },
                           [this] (const ParamReserve::Target& t) { const auto* set = targetSet (static_cast<int> (t.scope) - 1, static_cast<int> (t.sub)); return set->getNormalizedAt (set->indexOf (t.param)); });
        syncAssignments();
        return true;
    }

    // ---------------------------------------------------------------- fil audio
    // Latence totale (échantillons), à annoncer à l'hôte ; mise à jour à chaque bloc.
    int latencySamples() const noexcept { return latency_.load (std::memory_order_relaxed); }
    // Profil Live : tous les effets passent en variante à latence nulle.
    void setZeroLatency (bool zero) noexcept { zeroLatency_.store (zero, std::memory_order_relaxed); }

    // Traite en place.
    void process (float* left, float* right, int n) noexcept
    {
        for (int pos = 0; pos < n; pos += maxBlock_)
        {
            const int m = std::min (maxBlock_, n - pos);
            processBlock (left + pos, right + pos, m);
        }
    }

    // Niveau de crête du dernier bloc, par chemin (avant le volume du chemin) et en sortie, pour des vu-mètres.
    float pathPeak (int p) const noexcept { return p >= 0 && p < kPaths ? paths_[static_cast<std::size_t> (p)].peak.load (std::memory_order_relaxed) : 0.0f; }
    int pathLatency (int p) const noexcept { return p >= 0 && p < kPaths ? paths_[static_cast<std::size_t> (p)].latency.load (std::memory_order_relaxed) : 0; }
    const Crossover& crossover() const noexcept { return crossover_; }

private:
    // Ligne à retard stéréo, avec fondu enchaîné quand le retard change.
    struct Delay
    {
        void prepare (std::size_t capacity)
        {
            cap_ = std::max<std::size_t> (capacity, 16);
            l_.assign (cap_, 0.0f);
            r_.assign (cap_, 0.0f);
            pos_ = 0;
            cur_ = 0;
            fadeLeft_ = 0;
        }
        void clear() noexcept
        {
            std::fill (l_.begin(), l_.end(), 0.0f);
            std::fill (r_.begin(), r_.end(), 0.0f);
            fadeLeft_ = 0;
        }
        // Écrit puis relit `n` échantillons en place avec le retard voulu.
        void process (float* left, float* right, int n, int wanted) noexcept
        {
            wanted = std::clamp (wanted, 0, static_cast<int> (cap_) - 1);
            if (wanted != cur_ && fadeLeft_ == 0)
            {
                from_ = cur_;
                cur_ = wanted;
                fadeLeft_ = kFade;
            }
            for (int i = 0; i < n; ++i)
            {
                l_[pos_] = left[i];
                r_[pos_] = right[i];
                const std::size_t a = (pos_ + cap_ - static_cast<std::size_t> (cur_)) % cap_;
                float yl = l_[a], yr = r_[a];
                if (fadeLeft_ > 0)
                {
                    const float t = static_cast<float> (fadeLeft_) / static_cast<float> (kFade);   // 1 -> 0 : de l'ancien retard au nouveau
                    const std::size_t b = (pos_ + cap_ - static_cast<std::size_t> (from_)) % cap_;
                    yl = l_[b] * t + yl * (1.0f - t);
                    yr = r_[b] * t + yr * (1.0f - t);
                    --fadeLeft_;
                }
                left[i] = yl;
                right[i] = yr;
                pos_ = (pos_ + 1) % cap_;
            }
        }
        static constexpr int kFade = 1024;
        std::vector<float> l_, r_;
        std::size_t cap_ = 16, pos_ = 0;
        int cur_ = 0, from_ = 0, fadeLeft_ = 0;
    };
    struct Path
    {
        std::vector<float> bufL, bufR;
        Delay delay;
        float lastGain = 0.0f;
        bool gainInit = false;
        std::atomic<float> peak { 0.0f };
        std::atomic<int> latency { 0 };
    };

    static bool valid (int path, int slot) noexcept { return path >= 0 && path < kPaths && slot >= 0 && slot < kSlotsPerPath; }
    // Où se trouve le réglage visé : fil de messages (jeu de paramètres de l'instance publiée côté messages) ou fil audio (instance en service).
    ParamReserve::Ref resolveMsg (std::uint32_t scope, std::uint32_t sub, ParamId param) noexcept
    {
        const auto* set = targetSet (static_cast<int> (scope) - 1, static_cast<int> (sub));
        if (set == nullptr) return {};
        const auto idx = set->indexOf (param);
        return idx == ParameterSet::npos ? ParamReserve::Ref {} : ParamReserve::Ref { const_cast<ParameterSet*> (set), idx };
    }
    ParamReserve::Ref resolveAudio (std::uint32_t scope, std::uint32_t sub, ParamId param) noexcept
    {
        ParameterSet* set = nullptr;
        if (scope == 0) set = &params_;
        else if (valid (static_cast<int> (scope) - 1, static_cast<int> (sub)))
            if (auto* inst = const_cast<InsertInstance*> (slots_[scope - 1][sub].current())) set = &inst->runner.params();
        if (set == nullptr) return {};
        const auto idx = set->indexOf (param);
        return idx == ParameterSet::npos ? ParamReserve::Ref {} : ParamReserve::Ref { set, idx };
    }
    const ParameterSet* targetSet (int path, int slot) const noexcept
    {
        if (path < 0) return &params_;
        if (! valid (path, slot)) return nullptr;
        const auto& inst = msg_[static_cast<std::size_t> (path)][static_cast<std::size_t> (slot)];
        return inst != nullptr ? &inst->runner.params() : nullptr;
    }
    void publish (int path, int slot, std::shared_ptr<InsertInstance> inst)
    {
        msg_[static_cast<std::size_t> (path)][static_cast<std::size_t> (slot)] = inst;
        slots_[static_cast<std::size_t> (path)][static_cast<std::size_t> (slot)].publish (std::move (inst));
        collect();
        // une assignation dont la cible a disparu ou changé de type est retirée
        reserve_.prune ([this] (const ParamReserve::Target& t) { return resolveMsg (t.scope, t.sub, t.param).set != nullptr; });
    }

    static float dbToGain (double db) noexcept { return db <= -59.9 ? 0.0f : static_cast<float> (std::pow (10.0, db / 20.0)); }

    void applyAssignments() noexcept
    {
        reserve_.sync (params_, assignIdx_[0], [this] (std::uint32_t scope, std::uint32_t sub, ParamId param) { return resolveAudio (scope, sub, param); });
    }

    void processBlock (float* left, float* right, int n) noexcept
    {
        for (auto& row : slots_)
            for (auto& s : row) s.adoptPending();
        applyAssignments();

        // entrée
        const float inGain = static_cast<float> (std::pow (10.0, params_.getAt (idxIn_) / 20.0));
        {
            const float g0 = lastIn_ < 0.0f ? inGain : lastIn_;
            for (int i = 0; i < n; ++i)
            {
                const float g = g0 + (inGain - g0) * static_cast<float> (i + 1) / static_cast<float> (n);
                inL_[static_cast<std::size_t> (i)] = left[i] * g;
                inR_[static_cast<std::size_t> (i)] = right[i] * g;
            }
            lastIn_ = inGain;
        }

        // séparation en bandes (seulement si un chemin en a besoin)
        Source src[kPaths];
        bool needBands = false;
        for (int p = 0; p < kPaths; ++p)
        {
            src[p] = static_cast<Source> (std::clamp (static_cast<int> (params_.getAt (idxPath_[static_cast<std::size_t> (p)][pid::source])), 0, 3));
            needBands = needBands || src[p] != Source::Full;
        }
        if (needBands)
        {
            crossover_.setBands (params_.getAt (idxBands_) >= 0.5 ? 3 : 2);
            crossover_.setSlope (params_.getAt (idxSlope_) < 0.5 ? 12 : (params_.getAt (idxSlope_) < 1.5 ? 24 : 48));
            crossover_.setFrequencies (params_.getAt (idxX1_), params_.getAt (idxX2_));
            crossover_.process (inL_.data(), inR_.data(), lowL_.data(), lowR_.data(), midL_.data(), midR_.data(), highL_.data(), highR_.data(), n);
            for (int i = 0; i < n; ++i)
            {
                const auto iu = static_cast<std::size_t> (i);
                fullL_[iu] = lowL_[iu] + midL_[iu] + highL_[iu];
                fullR_[iu] = lowR_[iu] + midR_[iu] + highR_[iu];
            }
        }

        // chemins
        const bool zero = zeroLatency_.load (std::memory_order_relaxed);
        int maxLat = 0;
        int lat[kPaths] {};
        for (int p = 0; p < kPaths; ++p)
        {
            auto& path = paths_[static_cast<std::size_t> (p)];
            const std::vector<float>* sl = &inL_;
            const std::vector<float>* sr = &inR_;
            if (needBands)
                switch (src[p])
                {
                    case Source::Low: sl = &lowL_; sr = &lowR_; break;
                    case Source::Mid: sl = &midL_; sr = &midR_; break;
                    case Source::High: sl = &highL_; sr = &highR_; break;
                    default: sl = &fullL_; sr = &fullR_; break;
                }
            std::copy (sl->begin(), sl->begin() + n, path.bufL.begin());
            std::copy (sr->begin(), sr->begin() + n, path.bufR.begin());
            for (int s = 0; s < kSlotsPerPath; ++s)
                if (auto* inst = const_cast<InsertInstance*> (slots_[static_cast<std::size_t> (p)][static_cast<std::size_t> (s)].current()))
                {
                    inst->runner.setZeroLatency (zero);
                    inst->runner.process (path.bufL.data(), path.bufR.data(), n);
                    lat[p] += inst->runner.latencySamples();
                }
            path.latency.store (lat[p], std::memory_order_relaxed);
            maxLat = std::max (maxLat, lat[p]);
        }
        latency_.store (maxLat, std::memory_order_relaxed);

        // niveaux, retards d'alignement, somme
        bool anySolo = false;
        for (int p = 0; p < kPaths; ++p) anySolo = anySolo || params_.getAt (idxPath_[static_cast<std::size_t> (p)][pid::solo]) >= 0.5;
        std::fill (outL_.begin(), outL_.begin() + n, 0.0f);
        std::fill (outR_.begin(), outR_.begin() + n, 0.0f);
        for (int p = 0; p < kPaths; ++p)
        {
            auto& path = paths_[static_cast<std::size_t> (p)];
            const auto& ix = idxPath_[static_cast<std::size_t> (p)];
            float peak = 0.0f;
            for (int i = 0; i < n; ++i) peak = std::max ({ peak, std::fabs (path.bufL[static_cast<std::size_t> (i)]), std::fabs (path.bufR[static_cast<std::size_t> (i)]) });
            path.peak.store (peak, std::memory_order_relaxed);
            path.delay.process (path.bufL.data(), path.bufR.data(), n, maxLat - lat[p]);
            const bool muted = params_.getAt (ix[pid::mute]) >= 0.5 || (anySolo && params_.getAt (ix[pid::solo]) < 0.5);
            const float target = muted ? 0.0f : dbToGain (params_.getAt (ix[pid::gain])) * (params_.getAt (ix[pid::invert]) >= 0.5 ? -1.0f : 1.0f);
            if (! path.gainInit) { path.lastGain = target; path.gainInit = true; }
            const float g0 = path.lastGain;
            for (int i = 0; i < n; ++i)
            {
                const float g = g0 + (target - g0) * static_cast<float> (i + 1) / static_cast<float> (n);
                outL_[static_cast<std::size_t> (i)] += path.bufL[static_cast<std::size_t> (i)] * g;
                outR_[static_cast<std::size_t> (i)] += path.bufR[static_cast<std::size_t> (i)] * g;
            }
            path.lastGain = target;
        }

        // sortie
        const float outGain = dbToGain (params_.getAt (idxOut_));
        const float o0 = lastOut_ < 0.0f ? outGain : lastOut_;
        for (int i = 0; i < n; ++i)
        {
            const float g = o0 + (outGain - o0) * static_cast<float> (i + 1) / static_cast<float> (n);
            left[i] = outL_[static_cast<std::size_t> (i)] * g;
            right[i] = outR_[static_cast<std::size_t> (i)] * g;
        }
        lastOut_ = outGain;
    }

    ParameterSet params_;
    bool ok_ = true;
    std::size_t idxBands_ = 0, idxX1_ = 0, idxX2_ = 0, idxSlope_ = 0, idxIn_ = 0, idxOut_ = 0;
    std::array<std::array<std::size_t, 5>, kPaths> idxPath_ {};
    std::array<std::size_t, kAssignables> assignIdx_ {};
    double sampleRate_ = 48000.0;
    int maxBlock_ = 512;
    Crossover crossover_;
    std::vector<float> inL_, inR_, lowL_, lowR_, midL_, midR_, highL_, highR_, fullL_, fullR_, outL_, outR_;
    std::array<Path, kPaths> paths_;
    float lastIn_ = -1.0f, lastOut_ = -1.0f;
    std::atomic<int> latency_ { 0 };
    std::atomic<bool> zeroLatency_ { false };

    std::array<std::array<HandoffSlot<InsertInstance>, kSlotsPerPath>, kPaths> slots_;
    std::array<std::array<std::shared_ptr<InsertInstance>, kSlotsPerPath>, kPaths> msg_;   // fil de messages
    ParamReserve reserve_ { kAssignables };
};
} // namespace neptune::core::trident
