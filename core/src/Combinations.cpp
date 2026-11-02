#include "neptune/core/Combinations.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "neptune/core/Mixer.h"
#include "neptune/core/Modulators.h"
#include "neptune/core/NoteRouter.h"

namespace neptune::core::combi
{
using json = nlohmann::json;

const Memory* Library::general (int bank, int piston) const
{
    if (bank < 0 || bank >= kBanks || piston < 0 || piston >= kPistons) return nullptr;
    const auto it = generals.find (generalKey (bank, piston));
    return it != generals.end() && it->second.used ? &it->second : nullptr;
}

const Memory* Library::divisional (int track, int bank, int piston) const
{
    if (track < 0 || track >= kTracks || bank < 0 || bank >= kBanks || piston < 0 || piston >= kPistons) return nullptr;
    const auto it = divisionals.find (divisionalKey (track, bank, piston));
    return it != divisionals.end() && it->second.used ? &it->second : nullptr;
}

namespace
{
void cleanMemory (Memory& m)
{
    if (m.name.size() > kMaxName) m.name.resize (kMaxName);
    m.values.erase (std::remove_if (m.values.begin(), m.values.end(), [] (const auto& v) { return v.first == 0 || ! std::isfinite (v.second); }), m.values.end());
    if (m.values.size() > kMaxValues) m.values.resize (kMaxValues);
}

void cleanMap (std::map<int, Memory>& map, int keys)
{
    for (auto it = map.begin(); it != map.end();)
    {
        if (it->first < 0 || it->first >= keys || ! it->second.used) it = map.erase (it);
        else
        {
            cleanMemory (it->second);
            ++it;
        }
    }
}

bool numberOf (const json& o, const char* key, double& out)
{
    const auto f = o.find (key);
    if (f == o.end() || ! f->is_number()) return false;
    out = f->get<double>();
    return true;
}

json memoryToJson (const Memory& m)
{
    json o;
    if (! m.name.empty()) o["name"] = m.name;
    json values = json::array();
    for (const auto& [id, v] : m.values) values.push_back (json::array ({ id, v }));
    o["values"] = std::move (values);
    return o;
}

Memory memoryFromJson (const json& o)
{
    Memory m;
    m.used = true;
    if (const auto f = o.find ("name"); f != o.end() && f->is_string()) m.name = f->get<std::string>();
    if (const auto f = o.find ("values"); f != o.end() && f->is_array())
        for (const auto& v : *f)
            if (v.is_array() && v.size() == 2 && v[0].is_number_unsigned() && v[1].is_number() && m.values.size() < kMaxValues)
                m.values.emplace_back (v[0].get<ParamId>(), v[1].get<double>());
    cleanMemory (m);
    return m;
}
} // namespace

const char* templateKey (Template t) noexcept
{
    switch (t)
    {
        case Template::OrganConsole: return "organ_console";
        case Template::OrganStops: return "organ_stops";
        case Template::Mixer: return "mixer";
        case Template::Everything: return "everything";
        case Template::Custom: return "custom";
        default: return "organ_console";
    }
}

bool templateFromKey (const std::string& key, Template& out)
{
    for (int i = 0; i < static_cast<int> (Template::kCount); ++i)
        if (key == templateKey (static_cast<Template> (i))) { out = static_cast<Template> (i); return true; }
    return false;
}

Library sanitised (Library lib)
{
    cleanMap (lib.generals, numGeneralKeys);
    cleanMap (lib.divisionals, numDivisionalKeys);
    lib.sequence.erase (std::remove_if (lib.sequence.begin(), lib.sequence.end(), [] (int k) { return k < 0 || k >= numGeneralKeys; }), lib.sequence.end());
    if (lib.sequence.size() > static_cast<std::size_t> (kMaxSequence)) lib.sequence.resize (kMaxSequence);
    return lib;
}

std::string toJson (const Library& in)
{
    const Library lib = sanitised (in);
    json generals = json::array(), divisionals = json::array(), sequence = json::array();
    for (const auto& [key, m] : lib.generals)
    {
        json o = memoryToJson (m);
        o["bank"] = key / kPistons;
        o["piston"] = key % kPistons;
        generals.push_back (std::move (o));
    }
    for (const auto& [key, m] : lib.divisionals)
    {
        json o = memoryToJson (m);
        o["track"] = key / (kBanks * kPistons);
        o["bank"] = (key / kPistons) % kBanks;
        o["piston"] = key % kPistons;
        divisionals.push_back (std::move (o));
    }
    for (const int key : lib.sequence) sequence.push_back ({ { "bank", key / kPistons }, { "piston", key % kPistons } });
    json doc;
    if (lib.capture != CaptureSpec {})
    {
        json cap;
        cap["template"] = templateKey (lib.capture.tpl);
        if (lib.capture.tpl == Template::Custom)
        {
            cap["families"] = lib.capture.families;
            cap["scopes"] = lib.capture.scopes;
        }
        doc["capture"] = std::move (cap);
    }
    doc["generals"] = std::move (generals);
    doc["divisionals"] = std::move (divisionals);
    doc["sequence"] = std::move (sequence);
    return doc.dump();
}

Library fromJson (std::string_view text)
{
    Library lib;
    const auto doc = json::parse (text.begin(), text.end(), nullptr, false);
    if (! doc.is_object()) return lib;
    auto index = [] (const json& o, const char* key, int limit, int& out)
    {
        double d = 0.0;
        if (! numberOf (o, key, d) || d < 0.0 || d >= limit) return false;
        out = static_cast<int> (d);
        return true;
    };
    if (const auto it = doc.find ("capture"); it != doc.end() && it->is_object())
    {
        if (const auto t = it->find ("template"); t != it->end() && t->is_string()) templateFromKey (t->get<std::string>(), lib.capture.tpl);
        double f = 0.0, s = 0.0;
        if (numberOf (*it, "families", f) && f >= 0.0) lib.capture.families = static_cast<unsigned> (f);
        if (numberOf (*it, "scopes", s) && s >= 0.0) lib.capture.scopes = static_cast<unsigned> (s);
    }
    if (const auto it = doc.find ("generals"); it != doc.end() && it->is_array())
        for (const auto& o : *it)
        {
            int bank = 0, piston = 0;
            if (o.is_object() && index (o, "bank", kBanks, bank) && index (o, "piston", kPistons, piston))
                lib.generals[generalKey (bank, piston)] = memoryFromJson (o);
        }
    if (const auto it = doc.find ("divisionals"); it != doc.end() && it->is_array())
        for (const auto& o : *it)
        {
            int track = 0, bank = 0, piston = 0;
            if (o.is_object() && index (o, "track", kTracks, track) && index (o, "bank", kBanks, bank) && index (o, "piston", kPistons, piston))
                lib.divisionals[divisionalKey (track, bank, piston)] = memoryFromJson (o);
        }
    if (const auto it = doc.find ("sequence"); it != doc.end() && it->is_array())
        for (const auto& o : *it)
        {
            int bank = 0, piston = 0;
            if (o.is_object() && lib.sequence.size() < static_cast<std::size_t> (kMaxSequence) && index (o, "bank", kBanks, bank) && index (o, "piston", kPistons, piston))
                lib.sequence.push_back (generalKey (bank, piston));
        }
    return lib;
}

bool sequenceAdd (Library& lib, int key)
{
    if (key < 0 || key >= numGeneralKeys || lib.sequence.size() >= static_cast<std::size_t> (kMaxSequence)) return false;
    lib.sequence.push_back (key);
    return true;
}

bool sequenceRemove (Library& lib, int index)
{
    if (index < 0 || index >= static_cast<int> (lib.sequence.size())) return false;
    lib.sequence.erase (lib.sequence.begin() + index);
    return true;
}

bool sequenceMove (Library& lib, int index, int delta)
{
    const int other = index + delta;
    const int n = static_cast<int> (lib.sequence.size());
    if (delta == 0 || index < 0 || index >= n || other < 0 || other >= n) return false;
    std::swap (lib.sequence[static_cast<std::size_t> (index)], lib.sequence[static_cast<std::size_t> (other)]);
    return true;
}

bool eraseGeneral (Library& lib, int key)
{
    return lib.generals.erase (key) > 0;
}

namespace
{
bool renameIn (std::map<int, Memory>& memories, int key, std::string name)
{
    const auto it = memories.find (key);
    if (it == memories.end() || ! it->second.used) return false;
    const auto first = name.find_first_not_of (" \t\r\n");
    name = first == std::string::npos ? std::string() : name.substr (first, name.find_last_not_of (" \t\r\n") - first + 1);
    if (name.size() > kMaxName) name.resize (kMaxName);
    it->second.name = std::move (name);
    return true;
}
} // namespace

bool renameGeneral (Library& lib, int key, std::string name) { return renameIn (lib.generals, key, std::move (name)); }
bool renameDivisional (Library& lib, int key, std::string name) { return renameIn (lib.divisionals, key, std::move (name)); }
bool eraseDivisional (Library& lib, int key) { return lib.divisionals.erase (key) > 0; }

std::vector<ParamId> stopParams (const ParameterSet& set, int track)
{
    std::vector<ParamId> ids;
    if (track < 0 || track >= kTracks) return ids;
    for (int i = 0; i < kModulators; ++i)
    {
        const ParamId id = mixer::modulatorParam (track, i);
        if (set.indexOf (id) != ParameterSet::npos) ids.push_back (id);
    }
    return ids;
}

std::vector<ParamId> consoleParams (const ParameterSet& set)
{
    std::vector<ParamId> ids;
    for (int t = 0; t < kTracks; ++t)
    {
        const auto stops = stopParams (set, t);
        ids.insert (ids.end(), stops.begin(), stops.end());
    }
    for (int i = 0; i < router::kSwitches; ++i)
        if (const ParamId id = router::switchParam (i); set.indexOf (id) != ParameterSet::npos) ids.push_back (id);
    for (int i = 0; i < router::kMaxRules; ++i)
        if (const ParamId id = router::transposeParam (i); set.indexOf (id) != ParameterSet::npos) ids.push_back (id);
    return ids;
}

std::vector<ParamId> captureParams (const ParameterSet& set, const CaptureSpec& spec, int track)
{
    using paramgroups::Family;
    using paramgroups::Scope;
    const bool oneTrack = track >= 0 && track < kTracks;
    if (spec.tpl == Template::OrganConsole) return oneTrack ? stopParams (set, track) : consoleParams (set);
    if (spec.tpl == Template::OrganStops)
    {
        if (oneTrack) return stopParams (set, track);
        std::vector<ParamId> ids;
        for (int t = 0; t < kTracks; ++t)
        {
            const auto stops = stopParams (set, t);
            ids.insert (ids.end(), stops.begin(), stops.end());
        }
        return ids;
    }
    unsigned families = 0, scopes = 0;
    auto bit = [] (auto e) { return 1u << static_cast<unsigned> (e); };
    switch (spec.tpl)
    {
        case Template::Mixer:
            families = bit (Family::Strip) | bit (Family::Sends);
            scopes = bit (Scope::Track) | bit (Scope::Parallel) | bit (Scope::Bus) | bit (Scope::Master);
            break;
        case Template::Everything:
            for (int f = 0; f < paramgroups::numFamilies; ++f) if (static_cast<Family> (f) != Family::Other) families |= 1u << static_cast<unsigned> (f);
            for (int s = 0; s <= static_cast<int> (Scope::Global); ++s) scopes |= 1u << static_cast<unsigned> (s);
            break;
        default:
            families = spec.families;
            scopes = spec.scopes;
            break;
    }
    std::vector<ParamId> ids;
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        const auto& ps = set.spec (i);
        if (ps.hidden) continue;
        const auto g = paramgroups::classify (ps.name);
        if ((families & (1u << static_cast<unsigned> (g.family))) == 0 || (scopes & (1u << static_cast<unsigned> (g.scope))) == 0) continue;
        if (oneTrack && ! (g.scope == Scope::Track && g.index == track)) continue;   // une scène de piste : seulement les paramètres de cette piste
        if (ids.size() < kMaxValues) ids.push_back (ps.id);
    }
    return ids;
}

Memory capture (const ParameterSet& set, const std::vector<ParamId>& ids)
{
    Memory m;
    m.used = true;
    for (const ParamId id : ids)
    {
        const auto index = set.indexOf (id);
        if (index != ParameterSet::npos && m.values.size() < kMaxValues) m.values.emplace_back (id, set.getAt (index));
    }
    return m;
}

namespace
{
void apply (ParameterSet& set, ParamId id, double value, std::vector<Change>& changes, const Writer& write)
{
    const auto index = set.indexOf (id);
    if (index == ParameterSet::npos) return;
    const double before = set.getAt (index);
    const double target = set.spec (index).clamp (value);
    if (target == before) return;
    if (write) write (id, target);
    else set.setAt (index, target);
    const double after = set.getAt (index);
    if (after != before) changes.push_back ({ id, before, after });
}
} // namespace

std::vector<Change> recall (ParameterSet& set, const Memory& m, const Writer& write)
{
    std::vector<Change> changes;
    for (const auto& [id, value] : m.values) apply (set, id, value, changes, write);
    return changes;
}

std::vector<Change> cancelAll (ParameterSet& set, const Writer& write)
{
    std::vector<Change> changes;
    for (const ParamId id : consoleParams (set)) apply (set, id, 0.0, changes, write);
    return changes;
}

std::vector<Change> cancelAll (ParameterSet& set, const CaptureSpec& spec, const Writer& write)
{
    if (spec.tpl == Template::OrganConsole || spec.tpl == Template::OrganStops)
    {
        std::vector<Change> changes;
        for (const ParamId id : captureParams (set, spec, -1)) apply (set, id, 0.0, changes, write);
        return changes;
    }
    std::vector<Change> changes;
    for (const ParamId id : captureParams (set, spec, -1))
    {
        const auto index = set.indexOf (id);
        if (index != ParameterSet::npos) apply (set, id, set.spec (index).defaultValue, changes, write);   // comme à l'ouverture du logiciel
    }
    return changes;
}

bool matches (const ParameterSet& set, const Memory& m)
{
    bool any = false;
    for (const auto& [id, value] : m.values)
    {
        const auto index = set.indexOf (id);
        if (index == ParameterSet::npos) continue;
        any = true;
        if (set.getAt (index) != set.spec (index).clamp (value)) return false;
    }
    return any;
}

std::string commandKey (int command)
{
    if (command >= kCommandGeneral && command < kCommandGeneral + kPistons) return "combi_general_" + std::to_string (command - kCommandGeneral + 1);
    if (command >= kCommandDivisional && command < kCommandDivisional + kPistons) return "combi_divisional_" + std::to_string (command - kCommandDivisional + 1);
    switch (command)
    {
        case kCommandCancel: return "combi_cancel";
        case kCommandSet: return "combi_set";
        case kCommandNext: return "combi_next";
        case kCommandPrevious: return "combi_previous";
        case kCommandBankNext: return "combi_bank_next";
        case kCommandBankPrevious: return "combi_bank_previous";
        default: return {};
    }
}

std::string commandLabel (int command)
{
    if (command >= kCommandGeneral && command < kCommandGeneral + kPistons) return "G" + std::to_string (command - kCommandGeneral + 1);
    if (command >= kCommandDivisional && command < kCommandDivisional + kPistons) return "D" + std::to_string (command - kCommandDivisional + 1);
    switch (command)
    {
        case kCommandCancel: return "GC";
        case kCommandSet: return "Set";
        case kCommandNext: return "Next";
        case kCommandPrevious: return "Prev";
        case kCommandBankNext: return "Bank +";
        case kCommandBankPrevious: return "Bank -";
        default: return {};
    }
}

void Controller::setBank (int bank) noexcept { bank_ = std::clamp (bank, 0, kBanks - 1); }

bool Controller::nextBank() noexcept
{
    if (bank_ >= kBanks - 1) return false;
    ++bank_;
    return true;
}

bool Controller::previousBank() noexcept
{
    if (bank_ <= 0) return false;
    --bank_;
    return true;
}

namespace
{
Outcome storeInto (Memory& slot, Memory fresh)
{
    fresh.name = slot.name;   // l'enregistrement garde le nom existant
    slot = std::move (fresh);
    Outcome out;
    out.result = Result::Stored;
    return out;
}

Outcome recallMemory (const Memory* m, ParameterSet& set, const Writer& write)
{
    Outcome out;
    if (m == nullptr)
    {
        out.result = Result::Empty;
        return out;
    }
    out.result = Result::Recalled;
    out.changes = recall (set, *m, write);
    return out;
}
} // namespace

Outcome Controller::general (Library& lib, ParameterSet& set, int piston)
{
    if (piston < 0 || piston >= kPistons) return {};
    if (setActive())
    {
        armed_ = false;
        return storeInto (lib.generals[generalKey (bank_, piston)], capture (set, captureParams (set, lib.capture, -1)));
    }
    const int key = generalKey (bank_, piston);
    const auto it = std::find (lib.sequence.begin(), lib.sequence.end(), key);
    if (it != lib.sequence.end() && lib.general (bank_, piston) != nullptr) seq_ = static_cast<int> (it - lib.sequence.begin());
    return recallMemory (lib.general (bank_, piston), set, writer_);
}

Outcome Controller::divisional (Library& lib, ParameterSet& set, int track, int piston)
{
    if (track < 0 || track >= kTracks || piston < 0 || piston >= kPistons) return {};
    if (setActive())
    {
        armed_ = false;
        return storeInto (lib.divisionals[divisionalKey (track, bank_, piston)], capture (set, captureParams (set, lib.capture, track)));
    }
    return recallMemory (lib.divisional (track, bank_, piston), set, writer_);
}

Outcome Controller::cancel (ParameterSet& set)
{
    Outcome out;
    out.result = Result::Recalled;
    out.changes = cancelAll (set, writer_);
    return out;
}

Outcome Controller::cancel (const Library& lib, ParameterSet& set)
{
    Outcome out;
    out.result = Result::Recalled;
    out.changes = cancelAll (set, lib.capture, writer_);
    return out;
}

bool Controller::sequenceInsert (Library& lib, int key)
{
    if (key < 0 || key >= numGeneralKeys || lib.sequence.size() >= static_cast<std::size_t> (kMaxSequence)) return false;
    const int at = std::min (seq_ + 1, static_cast<int> (lib.sequence.size()));
    lib.sequence.insert (lib.sequence.begin() + at, key);
    seq_ = at;
    return true;
}

bool Controller::sequenceDeleteCurrent (Library& lib)
{
    if (seq_ < 0 || seq_ >= static_cast<int> (lib.sequence.size())) return false;
    lib.sequence.erase (lib.sequence.begin() + seq_);
    if (lib.sequence.empty()) seq_ = -1;
    else seq_ = std::min (seq_, static_cast<int> (lib.sequence.size()) - 1);
    return true;
}

Outcome Controller::recallKey (const Library& lib, ParameterSet& set, int key)
{
    return recallMemory (lib.general (key / kPistons, key % kPistons), set, writer_);
}

Outcome Controller::sequenceNext (const Library& lib, ParameterSet& set)
{
    if (seq_ + 1 >= static_cast<int> (lib.sequence.size())) return {};
    ++seq_;
    return recallKey (lib, set, lib.sequence[static_cast<std::size_t> (seq_)]);
}

Outcome Controller::sequencePrevious (const Library& lib, ParameterSet& set)
{
    if (seq_ <= 0 || lib.sequence.empty()) return {};
    seq_ = std::min (seq_, static_cast<int> (lib.sequence.size())) - 1;
    return recallKey (lib, set, lib.sequence[static_cast<std::size_t> (seq_)]);
}

Outcome Controller::run (int command, Library& lib, ParameterSet& set, int track)
{
    if (command >= kCommandGeneral && command < kCommandGeneral + kPistons) return general (lib, set, command - kCommandGeneral);
    if (command >= kCommandDivisional && command < kCommandDivisional + kPistons) return divisional (lib, set, track, command - kCommandDivisional);
    Outcome done;
    switch (command)
    {
        case kCommandCancel: return cancel (lib, set);
        case kCommandSet: armed_ = ! armed_; done.result = Result::Done; return done;
        case kCommandNext: return sequenceNext (lib, set);
        case kCommandPrevious: return sequencePrevious (lib, set);
        case kCommandBankNext: if (nextBank()) done.result = Result::Done; return done;
        case kCommandBankPrevious: if (previousBank()) done.result = Result::Done; return done;
        default: return done;
    }
}

Outcome Controller::programChange (Library& lib, ParameterSet& set, int program, int track)
{
    if (program >= 0 && program < kPistons) return general (lib, set, program);
    if (program >= kPistons && program < 2 * kPistons) return divisional (lib, set, track, program - kPistons);   // piste invalide : ignoré
    if (program >= 2 * kPistons && program < 2 * kPistons + kBanks)
    {
        Outcome out;
        const int bank = program - 2 * kPistons;
        if (bank != bank_) out.result = Result::Done;
        setBank (bank);
        return out;
    }
    return {};
}
} // namespace neptune::core::combi
