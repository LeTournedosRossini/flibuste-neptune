#include "neptune/core/Control.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <nlohmann/json.hpp>

#include "neptune/core/Combinations.h"

namespace neptune::core::control
{
using nlohmann::json;

// ---------------------------------------------------------------------------------------------------- actions, valeurs

static_assert (kNumCombinationActions == combi::kNumCommands, "une action par commande de combinaison");

namespace
{
const std::vector<std::string>& combinationKeys()
{
    static const std::vector<std::string> keys = []
    {
        std::vector<std::string> k;
        for (int c = 1; c <= kNumCombinationActions; ++c) k.push_back (combi::commandKey (c));
        return k;
    }();
    return keys;
}
} // namespace

const char* toString (Action a) noexcept
{
    if (const int command = combinationCommand (a); command >= 1 && command <= kNumCombinationActions) return combinationKeys()[static_cast<std::size_t> (command - 1)].c_str();
    switch (a)
    {
        case Action::PresetNext: return "preset_next";
        case Action::PresetPrevious: return "preset_previous";
        case Action::Panic: return "panic";
        case Action::TrackNext: return "track_next";
        case Action::TrackPrevious: return "track_previous";
        default: return "none";
    }
}

bool actionFromString (std::string_view s, Action& out)
{
    for (int i = 0; i < kNumActions; ++i)
        if (s == toString (static_cast<Action> (i))) { out = static_cast<Action> (i); return true; }
    return false;
}

double mapValue (const Binding& b, double s) noexcept
{
    double v = std::clamp (s, 0.0, 1.0);
    if (b.invert) v = 1.0 - v;
    if (b.curve != 1.0) v = std::pow (v, std::clamp (b.curve, 0.1, 10.0));
    const double lo = std::clamp (b.lo, 0.0, 1.0), hi = std::clamp (b.hi, 0.0, 1.0);
    return std::clamp (lo + (hi - lo) * v, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------------------- configuration

int relativeDelta (Mode mode, int v) noexcept
{
    v = std::clamp (v, 0, 127);
    switch (mode)
    {
        case Mode::RelativeTwos: return v == 0 ? 0 : (v < 64 ? v : v - 128);
        case Mode::RelativeOffset: return v - 64;
        case Mode::RelativeSign: return (v & 0x3F) * ((v & 0x40) != 0 ? -1 : 1);
        default: return 0;
    }
}

std::string sourceLabel (const Source& s)
{
    switch (s.kind)
    {
        case SourceKind::CC: return "CC " + std::to_string (s.number) + (s.channel > 0 ? " ch " + std::to_string (s.channel) : std::string());
        case SourceKind::Nrpn: return "NRPN " + std::to_string (s.number) + (s.channel > 0 ? " ch " + std::to_string (s.channel) : std::string());
        case SourceKind::Macro: return "Macro " + std::to_string (s.number + 1);
        case SourceKind::Osc: return "OSC " + s.address;
    }
    return {};
}

bool addMacroParams (ParameterSet& set, std::string* error)
{
    for (int i = 0; i < kMacros; ++i)
        if (! set.add (ParamSpec::continuous (macroParamId (i), "Macro " + std::to_string (i + 1), Unit::Percent, 0.0, 100.0, 0.0), error)) return false;
    return true;
}

bool isBandOrMacroParam (ParamId id) noexcept
{
    if (id >= kMacroBase && id < kMacroBase + static_cast<ParamId> (kMacros)) return true;
    if (id >= 1 && id <= 14) return true;                          // master
    // pistes (1000 + 100.t), bus (3000 + 100.b), pistes parallèles (5000 + 100.p) : décalages 0..3 seulement (volume, pan, mute, solo). Les envois, les bypass
    // d'inserts et le type de piste passent par les macros (clic droit : « Automatiser ce réglage »).
    const bool strip = (id >= 1000 && id < 2600) || (id >= 3000 && id < 4600) || (id >= 5000 && id < 8200);
    if (! strip) return false;
    return id % 100 <= 3;
}

Config sanitised (Config c)
{
    c.settings.oscPort = std::clamp (c.settings.oscPort, 1024, 65535);
    c.settings.feedbackPort = std::clamp (c.settings.feedbackPort, 0, 65535);
    if (c.settings.feedbackHost.size() > 253) c.settings.feedbackHost.clear();
    std::vector<Binding> kept;
    for (auto b : c.bindings)
    {
        if (static_cast<int> (kept.size()) >= kMaxBindings) break;
        switch (b.source.kind)
        {
            case SourceKind::CC:
                b.source.channel = std::clamp (b.source.channel, 0, 16);
                if (b.source.number < 0 || b.source.number > 127) continue;
                b.source.address.clear();
                break;
            case SourceKind::Nrpn:
                b.source.channel = std::clamp (b.source.channel, 0, 16);
                if (b.source.number < 0 || b.source.number > 16383) continue;
                b.source.address.clear();
                break;
            case SourceKind::Macro:
                if (b.source.number < 0 || b.source.number >= kMacros) continue;
                b.source.channel = 0;
                b.source.address.clear();
                break;
            case SourceKind::Osc:
                if (b.source.address.empty() || b.source.address.size() > static_cast<std::size_t> (kMaxAddressLength) || b.source.address[0] != '/') continue;
                b.source.channel = 0;
                b.source.number = 0;
                break;
        }
        if (b.target.isAction)
        {
            if (b.target.action == Action::None) continue;
            b.target.param = 0;
        }
        else
        {
            if (b.target.param == 0) continue;
            b.target.action = Action::None;
        }
        if (isRelative (b.mode) && (b.source.kind != SourceKind::CC || b.target.isAction)) b.mode = Mode::Absolute;   // relatif : CC vers un paramètre seulement
        b.lo = std::clamp (b.lo, 0.0, 1.0);
        b.hi = std::clamp (b.hi, 0.0, 1.0);
        b.curve = std::isfinite (b.curve) ? std::clamp (b.curve, 0.1, 10.0) : 1.0;
        kept.push_back (std::move (b));
    }
    c.bindings = std::move (kept);
    return c;
}

std::string toJson (const Config& c)
{
    json doc;
    doc["format"] = "neptune-control";
    doc["version"] = 1;
    doc["cc_enabled"] = c.settings.ccEnabled;
    doc["osc_enabled"] = c.settings.oscEnabled;
    doc["osc_port"] = c.settings.oscPort;
    if (c.settings.oscAllInterfaces) doc["osc_all_interfaces"] = true;
    if (! c.settings.feedbackHost.empty()) { doc["feedback_host"] = c.settings.feedbackHost; doc["feedback_port"] = c.settings.feedbackPort; }
    json list = json::array();
    for (const auto& b : c.bindings)
    {
        json o;
        json s;
        switch (b.source.kind)
        {
            case SourceKind::CC: s["type"] = "cc"; s["channel"] = b.source.channel; s["number"] = b.source.number; break;
            case SourceKind::Nrpn: s["type"] = "nrpn"; s["channel"] = b.source.channel; s["number"] = b.source.number; break;
            case SourceKind::Macro: s["type"] = "macro"; s["index"] = b.source.number; break;
            case SourceKind::Osc: s["type"] = "osc"; s["address"] = b.source.address; break;
        }
        o["source"] = std::move(s);
        if (b.target.isAction) o["target"] = { { "action", toString (b.target.action) } };
        else o["target"] = { { "param", b.target.param } };
        if (b.lo != 0.0) o["min"] = b.lo;
        if (b.hi != 1.0) o["max"] = b.hi;
        if (b.curve != 1.0) o["curve"] = b.curve;
        if (b.invert) o["invert"] = true;
        switch (b.mode)
        {
            case Mode::Pickup: o["mode"] = "pickup"; break;
            case Mode::RelativeTwos: o["mode"] = "relative_twos"; break;
            case Mode::RelativeOffset: o["mode"] = "relative_offset"; break;
            case Mode::RelativeSign: o["mode"] = "relative_sign"; break;
            default: break;
        }
        list.push_back (std::move (o));
    }
    doc["bindings"] = std::move (list);
    return doc.dump (2);
}

Config configFromJson (std::string_view text)
{
    Config c;
    const auto doc = json::parse (text.begin(), text.end(), nullptr, false);
    if (! doc.is_object()) return c;
    auto flag = [&doc] (const char* key, bool& out) { if (const auto it = doc.find (key); it != doc.end() && it->is_boolean()) out = it->get<bool>(); };
    auto integer = [&doc] (const char* key, int& out) { if (const auto it = doc.find (key); it != doc.end() && it->is_number_integer()) out = it->get<int>(); };
    flag ("cc_enabled", c.settings.ccEnabled);
    flag ("osc_enabled", c.settings.oscEnabled);
    flag ("osc_all_interfaces", c.settings.oscAllInterfaces);
    integer ("osc_port", c.settings.oscPort);
    integer ("feedback_port", c.settings.feedbackPort);
    if (const auto it = doc.find ("feedback_host"); it != doc.end() && it->is_string()) c.settings.feedbackHost = it->get<std::string>();
    if (const auto it = doc.find ("bindings"); it != doc.end() && it->is_array())
        for (const auto& o : *it)
        {
            if (! o.is_object()) continue;
            const auto s = o.find ("source"), t = o.find ("target");
            if (s == o.end() || ! s->is_object() || t == o.end() || ! t->is_object()) continue;
            Binding b;
            const auto type = s->find ("type");
            if (type == s->end() || ! type->is_string()) continue;
            const auto ty = type->get<std::string>();
            auto num = [] (const json& j, const char* key, int fallback) { const auto i = j.find (key); return i != j.end() && i->is_number_integer() ? i->get<int>() : fallback; };
            if (ty == "cc") { b.source.kind = SourceKind::CC; b.source.channel = num (*s, "channel", 0); b.source.number = num (*s, "number", -1); }
            else if (ty == "nrpn") { b.source.kind = SourceKind::Nrpn; b.source.channel = num (*s, "channel", 0); b.source.number = num (*s, "number", -1); }
            else if (ty == "macro") { b.source.kind = SourceKind::Macro; b.source.number = num (*s, "index", -1); }
            else if (ty == "osc")
            {
                b.source.kind = SourceKind::Osc;
                if (const auto a = s->find ("address"); a != s->end() && a->is_string()) b.source.address = a->get<std::string>();
            }
            else continue;
            if (const auto a = t->find ("action"); a != t->end() && a->is_string())
            {
                b.target.isAction = true;
                if (! actionFromString (a->get<std::string>(), b.target.action)) continue;
            }
            else if (const auto p = t->find ("param"); p != t->end() && p->is_number_integer() && p->get<long long>() > 0)
                b.target.param = static_cast<ParamId> (p->get<long long>());
            else continue;
            auto real = [&o] (const char* key, double& out) { if (const auto i = o.find (key); i != o.end() && i->is_number() && std::isfinite (i->get<double>())) out = i->get<double>(); };
            real ("min", b.lo);
            real ("max", b.hi);
            real ("curve", b.curve);
            if (const auto i = o.find ("invert"); i != o.end() && i->is_boolean()) b.invert = i->get<bool>();
            if (const auto i = o.find ("mode"); i != o.end() && i->is_string())
            {
                const auto m = i->get<std::string>();
                if (m == "pickup") b.mode = Mode::Pickup;
                else if (m == "relative_twos") b.mode = Mode::RelativeTwos;
                else if (m == "relative_offset") b.mode = Mode::RelativeOffset;
                else if (m == "relative_sign") b.mode = Mode::RelativeSign;
            }
            c.bindings.push_back (std::move (b));
            if (static_cast<int> (c.bindings.size()) >= kMaxBindings) break;
        }
    return sanitised (std::move (c));
}

// ---------------------------------------------------------------------------------------------------- moteur

// Tables de recherche immuables (sauf les états atomiques de reprise et de front montant).
struct ControlEngine::Runtime
{
    struct Item
    {
        Binding binding;
        std::size_t paramIndex = ParameterSet::npos;   // npos : paramètre inconnu (l'assignation n'agit pas), ou action
    };
    std::vector<Item> items;
    std::unique_ptr<std::atomic<bool>[]> pickedUp;     // par assignation « reprise »
    std::unique_ptr<std::atomic<bool>[]> wasHigh;      // par assignation d'action : la source était déjà haute (front montant)
    std::vector<std::uint16_t> ccStart;                // 16 voies x 128 numéros + 1
    std::vector<std::uint16_t> ccItems;
    std::array<std::vector<std::uint16_t>, kMacros> macroItems;
    std::map<std::string, std::vector<std::uint16_t>> osc;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> nrpn;   // (voie x 16384 + numéro, assignation), trié : recherche sans allocation
};

ControlEngine::ControlEngine() : dirty_ (new std::atomic<std::uint64_t>[kDirtyWords]())
{
    for (auto& q : queue_) q.store (0, std::memory_order_relaxed);
}

ControlEngine::~ControlEngine() = default;

void ControlEngine::configure (const Config& config, const ParameterSet& set)
{
    auto rt = std::make_shared<Runtime>();
    const Config clean = sanitised (config);
    for (const auto& b : clean.bindings)
    {
        Runtime::Item it;
        it.binding = b;
        if (! b.target.isAction) it.paramIndex = set.indexOf (b.target.param);
        rt->items.push_back (std::move (it));
    }
    const auto n = rt->items.size();
    rt->pickedUp.reset (new std::atomic<bool>[std::max<std::size_t> (n, 1)]());
    rt->wasHigh.reset (new std::atomic<bool>[std::max<std::size_t> (n, 1)]());

    // tableau direct voie x numéro (CSR) : l'audio ne parcourt que les assignations concernées
    std::vector<std::vector<std::uint16_t>> byCc (16 * 128);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& b = rt->items[i].binding;
        if (! b.target.isAction && rt->items[i].paramIndex == ParameterSet::npos) continue; // paramètre inconnu
        if (b.source.kind == SourceKind::CC)
        {
            const int first = b.source.channel == 0 ? 0 : b.source.channel - 1, last = b.source.channel == 0 ? 15 : b.source.channel - 1;
            for (int ch = first; ch <= last; ++ch) byCc[static_cast<std::size_t> (ch * 128 + b.source.number)].push_back (static_cast<std::uint16_t> (i));
        }
        else if (b.source.kind == SourceKind::Nrpn)
        {
            const int first = b.source.channel == 0 ? 0 : b.source.channel - 1, last = b.source.channel == 0 ? 15 : b.source.channel - 1;
            for (int ch = first; ch <= last; ++ch)
                rt->nrpn.emplace_back (static_cast<std::uint32_t> (ch * 16384 + b.source.number), static_cast<std::uint16_t> (i));
        }
        else if (b.source.kind == SourceKind::Macro) rt->macroItems[static_cast<std::size_t> (b.source.number)].push_back (static_cast<std::uint16_t> (i));
        else rt->osc[b.source.address].push_back (static_cast<std::uint16_t> (i));
    }
    std::sort (rt->nrpn.begin(), rt->nrpn.end());
    rt->ccStart.assign (16 * 128 + 1, 0);
    for (std::size_t k = 0; k < byCc.size(); ++k)
    {
        rt->ccStart[k] = static_cast<std::uint16_t> (rt->ccItems.size());
        rt->ccItems.insert (rt->ccItems.end(), byCc[k].begin(), byCc[k].end());
    }
    rt->ccStart[byCc.size()] = static_cast<std::uint16_t> (rt->ccItems.size());

    {
        const std::lock_guard<std::mutex> lock (mutex_);
        config_ = clean;
        shared_ = rt;
    }
    ccEnabled_.store (clean.settings.ccEnabled, std::memory_order_relaxed);
    slot_.publish (rt);
}

Config ControlEngine::config() const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    return config_;
}

Settings ControlEngine::settings() const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    return config_.settings;
}

void ControlEngine::collectGarbage() { slot_.collect(); }

void ControlEngine::resetPickup()
{
    std::shared_ptr<const Runtime> rt;
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        rt = shared_;
    }
    if (rt == nullptr) return;
    for (std::size_t i = 0; i < rt->items.size(); ++i) rt->pickedUp[i].store (false, std::memory_order_relaxed);
}

bool ControlEngine::takeLearnedCc (int& channel, int& number) noexcept
{
    const auto v = learned_.exchange (0, std::memory_order_acq_rel);
    if (v == 0) return false;
    channel = static_cast<int> ((v >> 8) & 0xFF);
    number = static_cast<int> (v & 0xFF);
    return true;
}

bool ControlEngine::takeLearnedNrpn (int& channel, int& number) noexcept
{
    const auto v = learnedNrpn_.exchange (0, std::memory_order_acq_rel);
    if (v == 0) return false;
    channel = static_cast<int> ((v >> 16) & 0xFF);
    number = static_cast<int> (v & 0xFFFF);
    return true;
}

bool ControlEngine::popAction (Action& out) noexcept
{
    const int v = queue_[qTail_ % kQueue].load (std::memory_order_acquire);
    if (v == 0) return false;
    queue_[qTail_ % kQueue].store (0, std::memory_order_release);
    ++qTail_;
    out = static_cast<Action> (v - 1);
    return true;
}

void ControlEngine::post (Action a) noexcept
{
    const auto head = qHead_.fetch_add (1, std::memory_order_acq_rel);
    auto& slot = queue_[head % kQueue];
    int expected = 0;
    slot.compare_exchange_strong (expected, static_cast<int> (a) + 1, std::memory_order_acq_rel); // file pleine : l'action est perdue
}

void ControlEngine::markChanged (std::size_t index) noexcept
{
    if (index < kDirtyWords * 64) dirty_[index >> 6].fetch_or (std::uint64_t (1) << (index & 63), std::memory_order_relaxed);
}

std::size_t ControlEngine::drainChanges (std::vector<std::size_t>& out)
{
    std::size_t count = 0;
    for (std::size_t w = 0; w < kDirtyWords; ++w)
    {
        std::uint64_t bits = dirty_[w].exchange (0, std::memory_order_acq_rel);
        for (std::size_t b = 0; bits != 0 && b < 64; ++b)
            if ((bits >> b) & 1u)
            {
                out.push_back (w * 64 + b);
                ++count;
            }
    }
    return count;
}

void ControlEngine::apply (const Runtime& rt, int item, double s, ParameterSet& set) noexcept
{
    const auto& it = rt.items[static_cast<std::size_t> (item)];
    const auto& b = it.binding;
    if (isRelative (b.mode) && ! b.target.isAction)
    {
        const int delta = relativeDelta (b.mode, static_cast<int> (std::lround (s * 127.0)));
        if (delta == 0) return;
        const double lo = std::min (b.lo, b.hi), hi = std::max (b.lo, b.hi);
        const double step = (hi - lo) / 127.0 * (b.invert ? -1.0 : 1.0);
        const double next = std::clamp (set.getNormalizedAt (it.paramIndex) + delta * step, lo, hi);
        if (set.setNormalizedAt (it.paramIndex, next)) markChanged (it.paramIndex);
        return;
    }
    if (b.target.isAction)
    {
        const bool high = s >= 0.5;
        const bool was = rt.wasHigh[static_cast<std::size_t> (item)].exchange (high, std::memory_order_relaxed);
        if (high && ! was) post (b.target.action);   // front montant : un seul déclenchement par appui
        return;
    }
    const double n = mapValue (b, s);
    if (b.mode == Mode::Pickup && ! rt.pickedUp[static_cast<std::size_t> (item)].load (std::memory_order_relaxed))
    {
        const double current = set.getNormalizedAt (it.paramIndex);
        if (std::fabs (n - current) > 0.02) return;    // pas encore au niveau du paramètre : on n'y touche pas
        rt.pickedUp[static_cast<std::size_t> (item)].store (true, std::memory_order_relaxed);
    }
    if (set.setNormalizedAt (it.paramIndex, n)) markChanged (it.paramIndex);
}

void ControlEngine::nrpnValue (int channel0, int number, int value14, ParameterSet& set) noexcept
{
    slot_.adoptPending();
    const Runtime* rt = slot_.current();
    if (rt == nullptr) return;
    const auto key = static_cast<std::uint32_t> (channel0 * 16384 + number);
    auto it = std::lower_bound (rt->nrpn.begin(), rt->nrpn.end(), std::make_pair (key, std::uint16_t (0)));
    for (; it != rt->nrpn.end() && it->first == key; ++it) apply (*rt, it->second, std::clamp (value14, 0, 16383) / 16383.0, set);
}

void ControlEngine::controlChange (int channel0, int number, int value, ParameterSet& set) noexcept
{
    if (channel0 < 0 || channel0 > 15 || number < 0 || number > 127) return;
    const bool learning = learning_.load (std::memory_order_relaxed);
    // NRPN : 99/98 choisissent le paramètre, 6/38 portent la valeur sur 14 bits, 100/101 (RPN) désélectionnent.
    const bool nrpnCc = number == 6 || number == 38 || number == 98 || number == 99 || number == 100 || number == 101;
    if (learning && ! nrpnCc)
        learned_.store ((1u << 24) | (static_cast<std::uint32_t> (channel0 + 1) << 8) | static_cast<std::uint32_t> (number & 0x7F), std::memory_order_release);
    if (nrpnCc)
    {
        auto& st = nrpn_[static_cast<std::size_t> (channel0)];
        switch (number)
        {
            case 99: st.msb = value; st.lsb = -1; st.dataMsb = st.dataLsb = 0; break;
            case 98: st.lsb = value; st.dataMsb = st.dataLsb = 0; break;
            case 100: case 101: st.msb = st.lsb = -1; break;
            default:
                if (number == 6) { st.dataMsb = value; st.dataLsb = 0; } else st.dataLsb = value;
                if (st.msb >= 0 && st.lsb >= 0 && ! (st.msb == 127 && st.lsb == 127))
                {
                    const int n = st.msb * 128 + st.lsb, v = st.dataMsb * 128 + st.dataLsb;
                    if (learning) learnedNrpn_.store ((1u << 24) | (static_cast<std::uint32_t> (channel0 + 1) << 16) | static_cast<std::uint32_t> (n), std::memory_order_release);
                    if (ccEnabled_.load (std::memory_order_relaxed)) nrpnValue (channel0, n, v, set);
                }
                break;
        }
    }
    if (! ccEnabled_.load (std::memory_order_relaxed)) return;
    slot_.adoptPending();
    const Runtime* rt = slot_.current();
    if (rt == nullptr) return;
    const auto key = static_cast<std::size_t> (channel0 * 128 + number);
    const double s = std::clamp (value, 0, 127) / 127.0;
    for (auto i = rt->ccStart[key]; i < rt->ccStart[key + 1]; ++i) apply (*rt, rt->ccItems[i], s, set);
}

void ControlEngine::macroChanged (int macro, double normalized, ParameterSet& set) noexcept
{
    slot_.adoptPending();
    const Runtime* rt = slot_.current();
    if (rt == nullptr || macro < 0 || macro >= kMacros) return;
    for (const auto i : rt->macroItems[static_cast<std::size_t> (macro)]) apply (*rt, i, normalized, set);
}

int ControlEngine::oscValue (const std::string& address, double normalized, ParameterSet& set) noexcept
{
    std::shared_ptr<const Runtime> rt;
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        rt = shared_;
    }
    if (rt == nullptr) return 0;
    const auto it = rt->osc.find (address);
    if (it == rt->osc.end()) return 0;
    for (const auto i : it->second) apply (*rt, i, normalized, set);
    return static_cast<int> (it->second.size());
}

bool ControlEngine::setParameter (ParamId id, double value, bool real, ParameterSet& set) noexcept
{
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos || ! std::isfinite (value)) return false;
    if (set.setAt (index, real ? value : set.spec (index).fromNormalized (std::clamp (value, 0.0, 1.0)))) markChanged (index);
    return true;
}

std::vector<std::string> ControlEngine::oscAddresses() const
{
    std::vector<std::string> out;
    const std::lock_guard<std::mutex> lock (mutex_);
    if (shared_ != nullptr)
        for (const auto& [address, items] : shared_->osc) out.push_back (address);
    return out;
}
} // namespace neptune::core::control
