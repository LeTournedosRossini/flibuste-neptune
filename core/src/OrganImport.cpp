#include "neptune/core/OrganImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include "neptune/core/Mixer.h"
#include "neptune/core/SampleMeta.h"
#include "neptune/core/StylePack.h"

namespace neptune::core::organimport
{
namespace fs = std::filesystem;

namespace
{
std::string lowered (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

std::string trimmed (std::string_view s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace (static_cast<unsigned char> (s[a]))) ++a;
    while (b > a && std::isspace (static_cast<unsigned char> (s[b - 1]))) --b;
    return std::string (s.substr (a, b - a));
}

bool validUtf8 (const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char> (s[i]);
        std::size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (std::size_t k = 1; k <= extra; ++k)
            if (i + k >= s.size() || (static_cast<unsigned char> (s[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}
} // namespace

std::string ensureUtf8 (std::string text)
{
    if (validUtf8 (text)) return text;
    std::string out;
    out.reserve (text.size() + text.size() / 8);
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char> (ch);
        if (c < 0x80) out.push_back (ch);
        else { out.push_back (static_cast<char> (0xC0 | (c >> 6))); out.push_back (static_cast<char> (0x80 | (c & 0x3F))); }
    }
    return out;
}

std::string numbered (std::string_view prefix, int number)
{
    char digits[16];
    std::snprintf (digits, sizeof digits, "%03d", number);
    return std::string (prefix) + digits;
}

IniFile IniFile::parse (std::string_view textIn)
{
    std::string text = ensureUtf8 (std::string (textIn));
    if (text.size() >= 3 && static_cast<unsigned char> (text[0]) == 0xEF && static_cast<unsigned char> (text[1]) == 0xBB && static_cast<unsigned char> (text[2]) == 0xBF) text.erase (0, 3);
    IniFile ini;
    std::string section;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        const auto eol = text.find ('\n', pos);
        const std::string line = trimmed (std::string_view (text).substr (pos, (eol == std::string::npos ? text.size() : eol) - pos));
        pos = eol == std::string::npos ? text.size() + 1 : eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[')
        {
            const auto close = line.find (']');
            if (close == std::string::npos) continue;
            section = trimmed (std::string_view (line).substr (1, close - 1));
            const std::string key = lowered (section);
            if (ini.data_.find (key) == ini.data_.end()) { ini.order_.push_back (section); ini.data_[key]; }
            continue;
        }
        const auto eq = line.find ('=');
        if (eq == std::string::npos || section.empty()) continue;
        std::string value = line.substr (eq + 1);
        for (std::size_t i = 1; i < value.size(); ++i)   // commentaire de fin de ligne : un « ; » précédé d'un espace
            if (value[i] == ';' && std::isspace (static_cast<unsigned char> (value[i - 1]))) { value.resize (i); break; }
        ini.data_[lowered (section)][lowered (trimmed (std::string_view (line).substr (0, eq)))] = trimmed (value);
    }
    return ini;
}

bool IniFile::hasSection (std::string_view section) const { return data_.find (lowered (std::string (section))) != data_.end(); }

bool IniFile::has (std::string_view section, std::string_view key) const
{
    const auto s = data_.find (lowered (std::string (section)));
    return s != data_.end() && s->second.find (lowered (std::string (key))) != s->second.end();
}

std::string IniFile::get (std::string_view section, std::string_view key, const std::string& def) const
{
    const auto s = data_.find (lowered (std::string (section)));
    if (s == data_.end()) return def;
    const auto k = s->second.find (lowered (std::string (key)));
    return k == s->second.end() ? def : k->second;
}

int IniFile::getInt (std::string_view section, std::string_view key, int def) const
{
    const std::string v = get (section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    const long n = std::strtol (v.c_str(), &end, 10);
    if (end == v.c_str()) return def;
    return static_cast<int> (std::clamp<long> (n, -1000000000L, 1000000000L));
}

double IniFile::getDouble (std::string_view section, std::string_view key, double def) const
{
    std::string v = get (section, key);
    if (v.empty()) return def;
    std::replace (v.begin(), v.end(), ',', '.');
    char* end = nullptr;
    const double d = std::strtod (v.c_str(), &end);
    return end == v.c_str() || ! std::isfinite (d) ? def : d;
}

bool IniFile::getBool (std::string_view section, std::string_view key, bool def) const
{
    const std::string v = get (section, key);
    if (v.empty()) return def;
    switch (std::tolower (static_cast<unsigned char> (v[0])))
    {
        case 'y': case '1': case 't': return true;
        case 'n': case '0': case 'f': return false;
        default: return def;
    }
}

namespace
{
// Fichier d'échantillon : chemin du fichier d'orgue (séparateurs Windows) retrouvé sur le disque, la casse des noms étant tolérée.
fs::path resolvePath (const fs::path& base, std::string relative)
{
    std::replace (relative.begin(), relative.end(), '\\', '/');
    const fs::path direct = base / fs::u8path (relative);
    std::error_code ec;
    if (fs::exists (direct, ec)) return direct;
    fs::path current = base;
    std::stringstream parts (relative);
    std::string part;
    while (std::getline (parts, part, '/'))
    {
        if (part.empty() || part == ".") continue;
        fs::path next = current / fs::u8path (part);
        if (! fs::exists (next, ec))
        {
            bool found = false;
            for (const auto& e : fs::directory_iterator (current, ec))
                if (lowered (e.path().filename().u8string()) == lowered (part)) { next = e.path(); found = true; break; }
            if (! found) return direct;
        }
        current = next;
    }
    return current;
}

// Dimensions d'un PNG (en-tête de 24 octets) ; faux pour tout autre fichier.
bool pngSize (const fs::path& path, int& width, int& height)
{
    std::ifstream in (path, std::ios::binary);
    unsigned char b[24] = {};
    if (! in.read (reinterpret_cast<char*> (b), 24)) return false;
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (std::memcmp (b, sig, 8) != 0) return false;
    auto be = [&b] (int o) { return static_cast<int> ((static_cast<unsigned> (b[o]) << 24) | (static_cast<unsigned> (b[o + 1]) << 16) | (static_cast<unsigned> (b[o + 2]) << 8) | b[o + 3]); };
    width = be (16);
    height = be (20);
    return width > 0 && height > 0 && width < 20000 && height < 20000;
}

struct FileMarks
{
    bool exists = false;
    std::vector<MetaLoop> loops;
    std::int64_t releaseMarker = -1;
};

// Mêmes noms que les clefs du fichier d'orgue : « sw007 » (interrupteur 7), « st101 » (le jeu 101 est son propre interrupteur), « cp003 » (idem pour un accouplement).
std::string switchKey (int number) { return "sw" + std::to_string (number); }

int combineOf (const std::string& fn, bool& known)
{
    const std::string f = lowered (fn);
    known = true;
    if (f.empty() || f == "and") return static_cast<int> (router::Combine::All);
    if (f == "or") return static_cast<int> (router::Combine::Any);
    if (f == "xor") return static_cast<int> (router::Combine::One);
    if (f == "nand") return static_cast<int> (router::Combine::NotAll);
    if (f == "nor") return static_cast<int> (router::Combine::None);
    known = false;
    return static_cast<int> (router::Combine::All);
}

// Préfixe de perspective d'un nom de rang (« Rear » dans « Rear: Ped Subbass 16 ») ; vide s'il n'y en a pas.
std::string rankPrefix (const std::string& rankName)
{
    const auto colon = rankName.find (':');
    return colon == std::string::npos || colon == 0 ? std::string() : trimmed (std::string_view (rankName).substr (0, colon));
}

// Préfixes qui sont des perspectives : ceux des rangs qu'un même jeu réunit quand ils diffèrent.
std::set<std::string> perspectivePrefixes (const IniFile& ini)
{
    std::set<std::string> out;
    for (const auto& section : ini.sections())
    {
        const std::string low = lowered (section);
        if (low.size() < 5 || low.rfind ("stop", 0) != 0 || low.find_first_not_of ("0123456789", 4) != std::string::npos) continue;
        const int n = ini.getInt (section, "NumberOfRanks", 0);
        std::set<std::string> here;
        for (int r = 1; r <= n; ++r)
        {
            const std::string prefix = rankPrefix (ini.get (numbered ("Rank", ini.getInt (section, numbered ("Rank", r), 0)), "Name"));
            if (! prefix.empty()) here.insert (prefix);
        }
        if (here.size() >= 2) out.insert (here.begin(), here.end());
    }
    return out;
}

class Builder
{
public:
    Builder (const IniFile& ini, const Options& options)
        : ini_ (ini), opt_ (options), perspectives_ (perspectivePrefixes (ini)), excluded_ (options.excludedPerspectives.begin(), options.excludedPerspectives.end()) {}

    Result run()
    {
        Result& r = result_;
        if (! ini_.hasSection ("Organ")) { r.error = "not an organ definition file (no [Organ] section)"; return r; }
        r.name = ini_.get ("Organ", "ChurchName", "Organ");
        organAmp_ = ini_.getInt ("Organ", "AmplitudeLevel", 100);
        organTuning_ = ini_.getDouble ("Organ", "PitchTuning", 0.0);
        organGain_ = ini_.getDouble ("Organ", "Gain", 0.0);
        const int manuals = ini_.getInt ("Organ", "NumberOfManuals", 0);
        const bool pedals = ini_.getBool ("Organ", "HasPedals", false);
        std::vector<int> indices;
        if (pedals) indices.push_back (0);
        for (int m = 1; m <= manuals; ++m) indices.push_back (m);
        if (indices.empty()) { r.error = "the organ has no keyboard"; return r; }
        // canaux MIDI : les manuels à la suite, puis le pédalier
        for (std::size_t i = 0; i < indices.size(); ++i)
        {
            const int m = indices[i];
            channelOf_[m] = std::clamp ((m == 0 ? opt_.firstChannel + manuals : opt_.firstChannel + m - 1), 1, 16);
        }
        const std::size_t room = static_cast<std::size_t> (std::max (0, mixer::numTracks - opt_.firstTrack));
        for (std::size_t i = 0; i < indices.size() && i < room; ++i) buildKeyboard (indices[i], i);
        if (indices.size() > room) warn ("the organ has more keyboards than Neptune has tracks: the extra ones are not imported");
        if (stopsLeftOut_ > 0) warn (std::to_string (stopsLeftOut_) + " stop(s) have no rank in the chosen perspectives and are left out");
        buildCouplers();
        buildCombinations();
        otherThings();
        buildFacade();
        for (const auto& [key, ids] : params_)
        {
            const bool on = initial_.count (key) != 0 && initial_.at (key);
            const auto level = levelOf_.find (key);
            for (const ParamId id : ids) r.initialValues.emplace_back (id, level != levelOf_.end() ? level->second : valueFor (id, on));
        }
        r.ok = true;
        return r;
    }

private:
    struct ManualInfo { int firstMidi = 36, accessible = 61, firstLogical = 1; std::size_t keyboard = 0; };


    // Le clavier qui porte le contrôle d'un réglage partagé : pour une boîte, le manuel dont le plus de jeux en dépendent ; sinon le premier manuel qui le porte ;
    // le pédalier seulement s'il est le seul.
    bool manualListsSwitch (int odfIndex, int number) const
    {
        const std::string section = numbered ("Manual", odfIndex);
        const int count = ini_.getInt (section, "NumberOfSwitches", 0);
        for (int j = 1; j <= count; ++j)
            if (ini_.getInt (section, numbered ("Switch", j), 0) == number) return true;
        return false;
    }

    std::size_t ownerOf (const std::string& key, bool box) const
    {
        std::size_t best = result_.keyboards.size();
        int bestScore = -1;
        for (std::size_t k = 0; k < modulators_.size(); ++k)
        {
            if (modulators_[k].find (key) == modulators_[k].end()) continue;
            int score = result_.keyboards[k].odfIndex != 0 ? 1000 : 0;   // un manuel prime sur le pédalier
            if (box)
            {
                const auto use = boxUse_.find ({ k, key });
                score += use != boxUse_.end() ? use->second : 0;
            }
            else
            {
                score -= static_cast<int> (k);
                if (key.rfind ("sw", 0) == 0 && manualListsSwitch (result_.keyboards[k].odfIndex, std::atoi (key.c_str() + 2))) score += 5000;   // le clavier dont le tirant fait partie
            }
            if (score > bestScore) { bestScore = score; best = k; }
        }
        return best;
    }

    // Une Façade sommaire : une page par clavier (ses jeux en boutons, le clavier en bas), puis une page des accouplements et des combinaisons.
    void buildFacade()
    {
        namespace fc = neptune::core::facade;
        constexpr int cols = 6, bw = 156, gap = 8, left = 12, top = 44;
        const int bh = opt_.stylePack.empty() ? 40 : 52;   // les plaquettes du pack sont en 3:1
        // Les pages générées s'habillent d'un pack de styles (s'il est installé : sinon Neptune garde son dessin d'origine) : un style par page et par sorte de contrôle.
        auto packStyle = [this] (fc::Facade& page, const std::string& kind) -> int
        {
            if (opt_.stylePack.empty()) return 0;
            const std::string name = "pack " + kind;
            for (const auto& st : page.styles) if (st.name == name) return st.id;
            auto ref = [this, &kind] (const char* file) { return neptune::core::stylepack::makeReference (opt_.stylePack, kind + "/" + file); };
            fc::ImageStyle st;
            st.id = page.freeStyleId();
            st.name = name;
            if (kind == "tablet" || kind == "piston") { st.off = ref ("off.svg"); st.on = ref ("on.svg"); st.textColour = "#2b1b0b"; st.textSize = kind == "tablet" ? 40 : 30; }
            else if (kind == "slider-h") { st.track = ref ("track.svg"); st.handle = ref ("handle.svg"); }
            else if (kind == "keys") { st.keyWhite = ref ("key_white.svg"); st.keyWhiteOn = ref ("key_white_on.svg"); st.keyBlack = ref ("key_black.svg"); st.keyBlackOn = ref ("key_black_on.svg"); st.keyLit = "#ffb347"; }
            else return 0;
            page.styles.push_back (st);
            return st.id;
        };
        fc::Facade main;
        std::vector<fc::Facade> pages;
        auto add = [] (fc::Facade& f, fc::WidgetType type, int x, int y, int w, int h) -> fc::Widget&
        {
            f.widgets.push_back (fc::defaultWidget (type, f.freeId(), x, y));
            auto& widget = f.widgets.back();
            widget.w = w;
            widget.h = h;
            return widget;
        };
        for (std::size_t k = 0; k < result_.keyboards.size(); ++k)
        {
            const auto& kb = result_.keyboards[k];
            fc::Facade page;
            page.name = kb.name.substr (0, 24);
            add (page, fc::WidgetType::Label, left, 4, 300, 32).text = kb.name;
            // Un réglage que plusieurs claviers portent (une boîte, le tirant du tremblant, « All Sounds ») n'a qu'un contrôle : sur le clavier qui l'« habite », les autres le suivent.
            int boxes = 0;
            for (const auto& box : boxControls_)
            {
                if (box.keyboard != k || ownerOf (box.key, true) != k) continue;
                auto& s = add (page, fc::WidgetType::Slider, left + (boxes % 3) * 326, 44 + (boxes / 3) * 36, 316, 32);
                ++boxes;
                s.param = mixer::modulatorParam (opt_.firstTrack + static_cast<int> (k), box.index);
                s.text = box.name;
                s.style = packStyle (page, "slider-h");
                for (const auto& other : boxControls_)
                    if (other.key == box.key && other.keyboard != k) s.mirror.push_back (mixer::modulatorParam (opt_.firstTrack + static_cast<int> (other.keyboard), other.index));
            }
            const int buttonTop = top + ((boxes + 2) / 3) * 36;   // les boutons des jeux sous les curseurs
            int n = 0;
            std::vector<std::pair<int, std::string>> order;
            for (const auto& entry : modulators_[k]) order.emplace_back (entry.second, entry.first);
            std::sort (order.begin(), order.end());
            for (const auto& [index, key] : order)
            {
                if (kb.instrument.modulators[static_cast<std::size_t> (index)].continuous) continue;   // boîtes (curseurs) et tremblants (via leur interrupteur) : pas de bouton marche/arrêt
                if (ownerOf (key, false) != k) continue;
                auto& b = add (page, fc::WidgetType::Button, left + (n % cols) * (bw + gap), buttonTop + (n / cols) * (bh + gap), bw, bh);
                b.param = mixer::modulatorParam (opt_.firstTrack + static_cast<int> (k), index);
                b.text = kb.instrument.modulators[static_cast<std::size_t> (index)].name;
                b.style = packStyle (page, "tablet");
                for (std::size_t other = 0; other < modulators_.size(); ++other)
                    if (other != k)
                        if (const auto it = modulators_[other].find (key); it != modulators_[other].end()) b.mirror.push_back (mixer::modulatorParam (opt_.firstTrack + static_cast<int> (other), it->second));
                ++n;
            }
            auto& keys = add (page, fc::WidgetType::Keyboard, left, 440, 976, 140);
            keys.channel = kb.midiChannel;
            keys.firstNote = 36;
            keys.hold = 0;
            keys.style = packStyle (page, "keys");
            (k == 0 ? main : pages.emplace_back()) = std::move (page);
        }
        std::vector<fc::Facade> panelPages;
        buildPanels (panelPages);
        fc::Facade last;
        last.name = "Couplers";
        add (last, fc::WidgetType::Label, left, 4, 500, 32).text = "Couplers";
        int n = 0;
        for (const auto& [label, id] : couplerControls_)
        {
            auto& b = add (last, fc::WidgetType::Button, left + (n % cols) * (bw + gap), top + (n / cols) * (bh + gap), bw, bh);
            b.param = id;
            b.text = label.substr (0, 40);
            b.style = packStyle (last, "tablet");
            ++n;
        }
        int y = 300;
        add (last, fc::WidgetType::Label, left, y - 40, 500, 32).text = "Generals / Divisionals";
        auto command = [&] (int x, int row, int cmd)
        {
            auto& b = add (last, fc::WidgetType::Action, x, y + row * (bh + gap), 100, bh);
            b.actionMode = 1 + cmd;
            b.style = packStyle (last, "piston");
        };
        int g = 0;
        for (int piston = 0; piston < combi::kFacadePistons; ++piston)
            if (result_.combinations.general (0, piston) != nullptr) command (left + (g++) * 108, 0, combi::kCommandGeneral + piston);
        command (left, 1, combi::kCommandCancel);
        command (left + 108, 1, combi::kCommandPrevious);
        command (left + 216, 1, combi::kCommandNext);
        int d = 0;
        for (int piston = 0; piston < combi::kFacadePistons; ++piston)
        {
            bool any = false;
            for (std::size_t k = 0; k < result_.keyboards.size(); ++k)
                if (result_.combinations.divisional (opt_.firstTrack + static_cast<int> (k), 0, piston) != nullptr) any = true;
            if (any) command (left + (d++) * 108, 2, combi::kCommandDivisional + piston);
        }
        // les panneaux de l'orgue d'abord (la console est la première page), puis les pages générées : curseurs de profondeur et de vitesse, claviers, accouplements
        std::vector<fc::Facade> book;
        for (auto& panel : panelPages) book.push_back (std::move (panel));
        if (! result_.keyboards.empty()) book.push_back (std::move (main));
        for (auto& page : pages) book.push_back (std::move (page));
        book.push_back (std::move (last));
        if (static_cast<int> (book.size()) > fc::maxPages) book.resize (static_cast<std::size_t> (fc::maxPages));
        main = std::move (book.front());
        book.erase (book.begin());
        main.pages = std::move (book);
        result_.facade = std::move (main);
    }

    // Un contrôle de l'orgue vu par la Façade : son paramètre principal (celui du clavier qui le porte) et les paramètres qui le suivent.
    bool resolveControl (const std::string& key, bool box, ParamId& primary, std::vector<ParamId>& mirrors) const
    {
        const auto it = params_.find (key);
        if (it == params_.end() || it->second.empty()) return false;
        primary = it->second.front();
        const std::size_t owner = ownerOf (key, box);
        if (owner < modulators_.size())
            if (const auto m = modulators_[owner].find (key); m != modulators_[owner].end())
                primary = mixer::modulatorParam (opt_.firstTrack + static_cast<int> (owner), m->second);
        mirrors.clear();
        for (const ParamId id : it->second) if (id != primary) mirrors.push_back (id);
        return true;
    }

    // Les panneaux de l'orgue (`[PanelNNN]` et leurs `[PanelNNNElementMMM]`) deviennent des pages de Façade : mêmes positions et mêmes images, mises à l'échelle du canevas
    // (échelle uniforme, centrée, comme l'image de fond ajustée). Un panneau dont les éléments n'ont pas de position (rangement automatique) est laissé aux pages générées.
    void buildPanels (std::vector<neptune::core::facade::Facade>& out)
    {
        namespace fc = neptune::core::facade;
        const int panelCount = ini_.getInt ("Organ", "NumberOfPanels", 0);
        std::set<std::string> unsupported;
        int unresolved = 0, autoLayout = 0, skippedWidgets = 0;
        for (int p = 0; p <= panelCount && static_cast<int> (out.size()) < 10; ++p)
        {
            const std::string ps = numbered ("Panel", p);
            if (! ini_.hasSection (ps)) continue;
            // taille du panneau : celle du fichier, sinon celle de l'image de fond
            fs::path background;
            int bgW = 0, bgH = 0;
            if (const std::string img = ini_.get (numbered (ps + "Image", 1), "Image"); ! img.empty())
            {
                const auto path = resolvePath (opt_.baseDir, img);
                if (! pngSize (path, bgW, bgH)) bgW = bgH = 0;
                else background = path;
            }
            int W = ini_.getInt (ps, "DispScreenSizeHoriz", -1), H = ini_.getInt (ps, "DispScreenSizeVert", -1);
            if (W <= 0 || H <= 0) { W = bgW > 0 ? bgW : fc::canvasWidth; H = bgH > 0 ? bgH : fc::canvasHeight; }
            const double scale = std::min (static_cast<double> (fc::canvasWidth) / W, static_cast<double> (fc::canvasHeight) / H);
            const double ox = (fc::canvasWidth - W * scale) / 2.0, oy = (fc::canvasHeight - H * scale) / 2.0;

            fc::Facade page;
            const std::string panelName = ini_.get (ps, "Name");
            page.name = (panelName.empty() ? "Panel " + std::to_string (p) : panelName).substr (0, 24);
            // l'image de fond n'est gardée que si elle a la taille du panneau (sinon c'est une texture que GrandOrgue répète, que la Façade ne répète pas)
            if (! background.empty() && std::abs (bgW - W) * 10 <= W && std::abs (bgH - H) * 10 <= H) { page.image = background.u8string(); page.fit = fc::ImageFit::Fit; }

            std::map<std::string, int> styleOf;
            auto styleFor = [&] (const std::string& on, const std::string& off) -> int
            {
                if (on.empty() && off.empty()) return 0;
                const auto onPath = on.empty() ? fs::path() : resolvePath (opt_.baseDir, on);
                const auto offPath = off.empty() ? fs::path() : resolvePath (opt_.baseDir, off);
                const std::string key = onPath.u8string() + "|" + offPath.u8string();
                if (const auto it = styleOf.find (key); it != styleOf.end()) return it->second;
                if (static_cast<int> (page.styles.size()) >= fc::maxStyles) return 0;
                fc::ImageStyle st;
                st.id = page.freeStyleId();
                st.name = fs::path (onPath).filename().u8string().substr (0, 30);
                st.on = onPath.u8string();
                st.off = offPath.u8string();
                page.styles.push_back (st);
                styleOf[key] = st.id;
                return st.id;
            };
            auto addWidget = [&] (fc::WidgetType type, int x, int y, int w, int h) -> fc::Widget*
            {
                if (static_cast<int> (page.widgets.size()) >= fc::maxWidgets) { ++skippedWidgets; return nullptr; }
                page.widgets.push_back (fc::defaultWidget (type, page.freeId(), 0, 0));
                auto& widget = page.widgets.back();
                widget.w = std::clamp (static_cast<int> (std::lround (w * scale)), fc::minWidgetSize, fc::canvasWidth);
                widget.h = std::clamp (static_cast<int> (std::lround (h * scale)), fc::minWidgetSize, fc::canvasHeight);
                widget.x = std::clamp (static_cast<int> (std::lround (ox + x * scale)), 0, fc::canvasWidth - widget.w);
                widget.y = std::clamp (static_cast<int> (std::lround (oy + y * scale)), 0, fc::canvasHeight - widget.h);
                return &widget;
            };
            auto sizeOf = [&] (const std::string& image, int& w, int& h, int defW, int defH)
            {
                w = defW; h = defH;
                if (image.empty()) return;
                int iw = 0, ih = 0;
                if (pngSize (resolvePath (opt_.baseDir, image), iw, ih)) { w = iw; h = ih; }
            };

            const int elements = ini_.getInt (ps, "NumberOfGUIElements", 0);
            bool anyAuto = false;
            for (int e = 1; e <= elements; ++e)
            {
                const std::string es = numbered (ps + "Element", e);
                if (! ini_.hasSection (es)) continue;
                const std::string type = lowered (ini_.get (es, "Type"));
                if (! ini_.has (es, "PositionX") && ! ini_.has (es, "PositionY")) { anyAuto = true; continue; }
                const int x = ini_.getInt (es, "PositionX", 0), y = ini_.getInt (es, "PositionY", 0);
                const std::string imageOn = ini_.get (es, "ImageOn"), imageOff = ini_.get (es, "ImageOff");
                const std::string label = ini_.get (es, "DispLabelText");
                int w = 0, h = 0;
                sizeOf (imageOn.empty() ? imageOff : imageOn, w, h, 120, 44);

                std::string controlKey;
                bool box = false;
                if (type == "switch") controlKey = switchKey (ini_.getInt (es, "Switch", 0));
                else if (type == "stop") controlKey = "st" + std::to_string (ini_.getInt (es, "Stop", 0));
                else if (type == "coupler") controlKey = "cp" + std::to_string (ini_.getInt (es, "Coupler", 0));
                else if (type == "tremulant")
                {
                    const int n = ini_.getInt (es, "Tremulant", 0);
                    const std::string ts = numbered ("Tremulant", n);
                    const int sw = ini_.getInt (ts, "Switch001", 0);
                    controlKey = ini_.getInt (ts, "SwitchCount", 0) > 0 && sw > 0 ? switchKey (sw) : "tm" + std::to_string (n);
                }
                if (! controlKey.empty())
                {
                    ParamId primary = 0;
                    std::vector<ParamId> mirrors;
                    if (! resolveControl (controlKey, false, primary, mirrors)) { ++unresolved; continue; }
                    auto* widget = addWidget (fc::WidgetType::Button, x, y, w, h);
                    if (widget == nullptr) continue;
                    widget->param = primary;
                    widget->mirror = mirrors;
                    widget->style = styleFor (imageOn, imageOff);
                    if (widget->style == 0) widget->text = label.empty() ? controlKey : label.substr (0, 40);
                }
                else if (type == "enclosure")
                {
                    ParamId primary = 0;
                    std::vector<ParamId> mirrors;
                    if (! resolveControl ("en" + std::to_string (ini_.getInt (es, "Enclosure", 0)), true, primary, mirrors)) { ++unresolved; continue; }
                    sizeOf (ini_.get (es, "Bitmap001"), w, h, 60, 200);
                    auto* widget = addWidget (fc::WidgetType::Slider, x, y, w, h);
                    if (widget == nullptr) continue;
                    widget->param = primary;
                    widget->mirror = mirrors;
                    widget->vertical = h >= w ? 1 : 0;
                    widget->text = label;
                }
                else if (type == "manual")
                {
                    const int manual = ini_.getInt (es, "Manual", 0);
                    const std::string ms = numbered ("Manual", manual);
                    std::size_t index = result_.keyboards.size();
                    for (std::size_t k = 0; k < result_.keyboards.size(); ++k) if (result_.keyboards[k].odfIndex == manual) index = k;
                    if (index >= result_.keyboards.size()) { ++unresolved; continue; }
                    int keysWidth = 0, keysHeight = 0, widths = 0;
                    const int keyCount = ini_.getInt (ms, "NumberOfAccessibleKeys", ini_.getInt (ms, "NumberOfLogicalKeys", 0));
                    for (int k = 1; k <= std::max (keyCount, 1) + 24 && ini_.has (es, numbered ("Key", k) + "Width") ; ++k)
                    {
                        keysWidth += ini_.getInt (es, numbered ("Key", k) + "Width", 0);
                        ++widths;
                        int kw = 0, kh = 0;
                        sizeOf (ini_.get (es, numbered ("Key", k) + "ImageOff"), kw, kh, 0, 0);
                        keysHeight = std::max (keysHeight, kh);
                    }
                    // des largeurs données pour quelques touches seulement (le reste vient des images) ne disent pas la largeur du clavier : une largeur ordinaire
                    if (widths * 2 < keyCount) keysWidth = 0;
                    auto* widget = addWidget (fc::WidgetType::Keyboard, x, y, keysWidth > 0 ? keysWidth : 600, keysHeight > 0 ? keysHeight : 100);
                    if (widget == nullptr) continue;
                    widget->channel = result_.keyboards[index].midiChannel;
                    widget->firstNote = std::clamp (ini_.getInt (ms, "FirstAccessibleKeyMIDINoteNumber", 36), 0, 127);
                    widget->hold = 0;
                }
                else if (type.rfind ("general", 0) == 0 && type.size() > 7)
                {
                    const int piston = std::atoi (type.c_str() + 7) - 1;
                    if (piston < 0 || piston >= combi::kFacadePistons || result_.combinations.general (0, piston) == nullptr) { ++unresolved; continue; }
                    auto* widget = addWidget (fc::WidgetType::Action, x, y, w, h);
                    if (widget == nullptr) continue;
                    widget->actionMode = 1 + combi::kCommandGeneral + piston;
                    widget->style = styleFor (imageOn, imageOff);
                    widget->text = std::to_string (piston + 1);
                }
                else if (type == "gc")   // « General Cancel » : l'annulation générale
                {
                    auto* widget = addWidget (fc::WidgetType::Action, x, y, w, h);
                    if (widget == nullptr) continue;
                    widget->actionMode = 1 + combi::kCommandCancel;
                    widget->style = styleFor (imageOn, imageOff);
                    widget->text = label.empty() ? "GC" : label.substr (0, 20);
                }
                else if (type == "set")
                {
                    auto* widget = addWidget (fc::WidgetType::Set, x, y, w, h);
                    if (widget == nullptr) continue;
                    widget->style = styleFor (imageOn, imageOff);
                    widget->text = label.empty() ? "Set" : label.substr (0, 20);
                }
                else if (type == "label")
                {
                    auto* widget = addWidget (fc::WidgetType::Label, x, y, std::max (w, 160), std::max (h, 32));
                    if (widget != nullptr) widget->text = label.substr (0, 60);
                }
                else unsupported.insert (type);
            }
            // les claviers par-dessus le reste : une image de panneau qui les recouvre les cacherait
            std::stable_partition (page.widgets.begin(), page.widgets.end(), [] (const fc::Widget& w) { return w.type != fc::WidgetType::Keyboard; });
            // un panneau dont une partie se range toute seule n'est pas reproduit à moitié : ses éléments restent aux pages générées
            if (anyAuto) { ++autoLayout; continue; }
            if (page.widgets.size() >= 2) out.push_back (std::move (page));
        }
        if (out.empty() && ini_.getInt ("Organ", "NumberOfPanels", 0) > 0) warn ("no graphical panel could be reproduced: the generated pages are used");
        if (autoLayout > 0) warn (std::to_string (autoLayout) + " panel(s) place their elements automatically (no PositionX/Y): left to the generated pages");
        if (unresolved > 0) warn (std::to_string (unresolved) + " panel element(s) act on nothing that was imported: skipped");
        if (skippedWidgets > 0) warn (std::to_string (skippedWidgets) + " panel element(s) over the 200 widgets of a Facade page: skipped");
        if (! unsupported.empty())
        {
            std::string kinds;
            for (const auto& k : unsupported) kinds += (kinds.empty() ? "" : ", ") + k;
            warn ("panel element types not shown: " + kinds);
        }
    }

    void warn (std::string text) { if (result_.warnings.size() < 200) result_.warnings.push_back (std::move (text)); }

    static double valueFor (ParamId id, bool on)
    {
        const bool tirant = id >= router::switchBase && id < router::switchBase + static_cast<ParamId> (router::kSwitches);
        return on ? (tirant ? 1.0 : 100.0) : 0.0;
    }

    std::string switchName (const std::string& key, const std::string& fallback)
    {
        if (key.rfind ("sw", 0) == 0)
        {
            const std::string name = ini_.get (numbered ("Switch", std::atoi (key.c_str() + 2)), "Name");
            if (! name.empty()) return name;
        }
        return fallback;
    }

    // La clef d'un interrupteur et son état initial.
    void noteInitial (const std::string& key, bool engaged) { initial_[key] = engaged; }

    int modulatorFor (std::size_t keyboard, const std::string& key, const std::string& name)
    {
        auto& table = modulators_[keyboard];
        const auto it = table.find (key);
        if (it != table.end()) return it->second;
        const int index = static_cast<int> (table.size());
        if (index >= kModulators) return -1;
        table[key] = index;
        result_.keyboards[keyboard].instrument.modulators[static_cast<std::size_t> (index)].name = name.substr (0, 40);
        params_[key].push_back (mixer::modulatorParam (opt_.firstTrack + static_cast<int> (keyboard), index));
        return index;
    }

    // Le paramètre qui représente un interrupteur dans une condition du routeur : le premier modulateur qui le porte, sinon un tirant du routeur.
    ParamId paramFor (const std::string& key)
    {
        const auto it = params_.find (key);
        if (it != params_.end() && ! it->second.empty()) return it->second.front();
        if (routerSwitches_ >= router::kSwitches) return 0;
        const ParamId id = router::switchParam (routerSwitches_++);
        params_[key].push_back (id);
        return id;
    }

    const FileMarks& marksOf (const fs::path& path)
    {
        const std::string key = path.u8string();
        const auto it = marks_.find (key);
        if (it != marks_.end()) return it->second;
        FileMarks m;
        std::error_code ec;
        m.exists = fs::exists (path, ec);
        if (! m.exists) { if (opt_.readSampleMarks && ++result_.missingSamples <= 2) warn ("sample not found: " + key); }
        else if (opt_.readSampleMarks)
        {
            SampleMeta meta;
            if (readFileMarks (path, meta)) m.loops = meta.loops;
            std::int64_t cue = -1;
            if (readReleaseMarker (path, cue)) m.releaseMarker = cue;
        }
        return marks_.emplace (key, std::move (m)).first->second;
    }

    // « REF:manuel:jeu:tuyau » : le tuyau est celui d'un autre jeu (le jeu = sa place dans le clavier, 1..). Il garde les réglages du tuyau visé (fichier, accord, volume).
    bool followReference (std::string& section, int& pipe, double& ampFactor, double& tuning, double& gainDb)
    {
        for (int depth = 0; depth < 6; ++depth)
        {
            const std::string value = ini_.get (section, numbered ("Pipe", pipe));
            if (value.rfind ("REF:", 0) != 0) return true;
            int manual = -1, stop = 0, target = 0;
            if (std::sscanf (value.c_str(), "REF:%d:%d:%d", &manual, &stop, &target) != 3) return false;
            const std::string manualSection = numbered ("Manual", manual);
            const int stopNumber = ini_.getInt (manualSection, numbered ("Stop", stop), 0);
            const std::string stopSection = numbered ("Stop", stopNumber);
            if (stopNumber <= 0 || ! ini_.hasSection (stopSection)) return false;
            ampFactor = organAmp_ / 100.0 * ini_.getInt (stopSection, "AmplitudeLevel", 100) / 100.0;
            tuning = organTuning_ + ini_.getDouble (stopSection, "PitchTuning", 0.0);
            gainDb = organGain_ + ini_.getDouble (stopSection, "Gain", 0.0);
            section = stopSection;
            pipe = target;
            if (ini_.getInt (stopSection, "NumberOfRanks", 0) > 0)   // un jeu à rangs : le tuyau est celui de son premier rang
            {
                const std::string rankSection = numbered ("Rank", ini_.getInt (stopSection, numbered ("Rank", 1), 0));
                if (! ini_.hasSection (rankSection)) return false;
                ampFactor *= ini_.getInt (rankSection, "AmplitudeLevel", 100) / 100.0;
                tuning += ini_.getDouble (rankSection, "PitchTuning", 0.0);
                gainDb += ini_.getDouble (rankSection, "Gain", 0.0);
                section = rankSection;
            }
        }
        return false;
    }

    void addPipe (Keyboard& kb, const ManualInfo& info, std::string section, int pipe, int logicalKey, const std::string& group, double ampFactor, double tuning, double gainDb)
    {
        if (ini_.get (section, numbered ("Pipe", pipe)).rfind ("REF:", 0) == 0 && ! followReference (section, pipe, ampFactor, tuning, gainDb))
        {
            warn ("keyboard \"" + kb.name + "\": a pipe reference of group \"" + group + "\" cannot be resolved");
            return;
        }
        const std::string p = numbered ("Pipe", pipe);
        const std::string file = ini_.get (section, p);
        if (file.empty() || file.rfind ("REF:", 0) == 0 || lowered (file) == "dummy") return;
        if (logicalKey < info.firstLogical || logicalKey >= info.firstLogical + info.accessible) return;   // une touche que le clavier n'a pas
        const int note = info.firstMidi + (logicalKey - info.firstLogical);
        if (note < 0 || note > 127) return;
        const double amp = ampFactor * ini_.getInt (section, p + "AmplitudeLevel", 100) / 100.0;
        const double tuneCents = std::clamp (tuning + ini_.getDouble (section, p + "PitchTuning", 0.0), -1200.0, 1200.0);
        const double gainTotal = std::clamp (20.0 * std::log10 (std::max (amp, 1.0e-6)) + gainDb + ini_.getDouble (section, p + "Gain", 0.0), -60.0, 60.0);
        // Les variantes d'attaque : le fichier du tuyau (vélocité minimale 0) puis `PipeNNNAttackMMM`. Comme GrandOrgue, une variante est possible quand la vélocité
        // atteint sa vélocité minimale, et le choix parmi les possibles est au hasard ; la plage de vélocité 1..127 est donc coupée aux vélocités minimales, chaque
        // tranche ayant son jeu de variantes tirées au hasard (round-robin aléatoire).
        struct Variant { std::string file; int minVelocity = 0; std::string prefix; };
        std::vector<Variant> variants { { file, 0, p } };
        const int attackCount = std::clamp (ini_.getInt (section, p + "AttackCount", 0), 0, 100);
        for (int a = 1; a <= attackCount; ++a)
        {
            const std::string prefix = p + numbered ("Attack", a);
            const std::string attackFile = ini_.get (section, prefix);
            if (attackFile.empty() || lowered (attackFile) == "dummy") continue;
            if (ini_.getInt (section, prefix + "MaxTimeSinceLastRelease", -1) >= 0) { ++repeatAttacksSkipped_; continue; }   // attaque d'un tuyau qui vient d'être relâché : pas de mesure de ce délai
            if (ini_.getInt (section, prefix + "IsTremulant", -1) == 1) continue;   // enregistrée avec un tremblant d'échantillons, non importé
            variants.push_back ({ attackFile, std::clamp (ini_.getInt (section, prefix + "AttackVelocity", 0), 0, 127), prefix });
        }
        // Les relâchements enregistrés à part : `PipeNNNReleaseMMM`, choisis selon la durée d'appui (`MaxKeyPressTime`, -1 : sans limite). Comme GrandOrgue, c'est le plus court qui
        // convient à la durée d'appui qui est joué ; chaque durée maximale ouvre donc une tranche (tranche précédente + 1 ms .. durée maximale). Quand un tuyau en a, la queue
        // du fichier d'attaque ne joue plus (sinon deux relâchements à chaque note).
        struct Release { std::string file; int maxMs = -1; std::int64_t cue = -1; std::int64_t end = -1; int crossfadeMs = 0; };
        std::vector<Release> releases;
        const int releaseCount = std::clamp (ini_.getInt (section, p + "ReleaseCount", 0), 0, 100);
        for (int r = 1; r <= releaseCount; ++r)
        {
            const std::string prefix = p + numbered ("Release", r);
            const std::string releaseFile = ini_.get (section, prefix);
            if (releaseFile.empty() || lowered (releaseFile) == "dummy") continue;
            if (ini_.getInt (section, prefix + "IsTremulant", -1) == 1) continue;   // enregistré avec un tremblant d'échantillons, non importé
            releases.push_back ({ releaseFile, ini_.getInt (section, prefix + "MaxKeyPressTime", -1), ini_.getInt (section, prefix + "CuePoint", -1),
                                  ini_.getInt (section, prefix + "ReleaseEnd", -1), std::clamp (ini_.getInt (section, prefix + "ReleaseCrossfadeLength", 0), 0, 3000) });
        }
        const bool ownRelease = ! releases.empty();
        std::vector<int> cuts;
        for (const auto& v : variants) if (v.minVelocity > 0) cuts.push_back (v.minVelocity);
        std::sort (cuts.begin(), cuts.end());
        cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());
        cuts.insert (cuts.begin(), 1);
        for (std::size_t slice = 0; slice < cuts.size(); ++slice)
        {
            const int velLow = cuts[slice], velHigh = slice + 1 < cuts.size() ? cuts[slice + 1] - 1 : 127;
            int rank = 0;
            std::vector<std::size_t> members;
            for (const auto& variant : variants)
            {
                if (variant.minVelocity > velLow) continue;
                ZoneDescription z;
                const fs::path path = resolvePath (opt_.baseDir, variant.file);
                z.samplePath = path.u8string();
                z.keyLow = z.keyHigh = note;
                z.rootNote = note;   // l'échantillon joue à sa hauteur d'origine (comme l'accord « original » de GrandOrgue) ; l'accord du tuyau s'y ajoute
                z.tuneCents = tuneCents;
                z.gainDb = gainTotal;
                z.group = group;
                z.velLow = velLow;
                z.velHigh = velHigh;
                const auto& marks = marksOf (path);
                // les boucles écrites dans le fichier d'orgue (`PipeNNNLoopCount`, `LoopMMMStart` / `End`) priment sur celles du fichier d'échantillon
                std::vector<MetaLoop> odfLoops;
                for (int l = 1, n = std::clamp (ini_.getInt (section, variant.prefix + "LoopCount", 0), 0, 100); l <= n; ++l)
                {
                    const std::string lp = variant.prefix + numbered ("Loop", l);
                    MetaLoop loop;
                    loop.start = ini_.getInt (section, lp + "Start", 0);
                    loop.end = ini_.getInt (section, lp + "End", -1);
                    if (loop.end > loop.start) odfLoops.push_back (loop);
                }
                const auto& loops = odfLoops.empty() ? marks.loops : odfLoops;
                if (! opt_.readSampleMarks || ! odfLoops.empty() || (marks.exists && ! marks.loops.empty()))   // sans lecture des fichiers : un tuyau d'orgue se tient (boucle)
                {
                    z.mode = PlayMode::Loop;
                    if (! loops.empty())
                    {
                        z.loopStart = loops[0].start;
                        z.loopEnd = loops[0].end;
                        if (! odfLoops.empty()) z.loopCrossfade = std::max (0, ini_.getInt (section, variant.prefix + "LoopCrossfadeLength", 0));
                        else z.loopCrossfade = loops[0].crossfade;
                        for (std::size_t i = 1; i < loops.size() && z.extraLoops.size() < ZoneDescription::maxExtraLoops; ++i)
                            z.extraLoops.push_back ({ loops[i].start, loops[i].end, loops[i].crossfade, loops[i].repeats });
                        const std::int64_t lastEnd = loops.back().end;
                        // `LoadRelease=N` : le fichier n'a pas de queue à jouer (GrandOrgue ne charge pas la fin du fichier comme relâchement) ; par défaut oui
                        const bool loadRelease = ini_.getBool (section, variant.prefix + "LoadRelease", true);
                        if (! ownRelease && loadRelease && marks.releaseMarker > 0 && lastEnd >= 0 && marks.releaseMarker >= lastEnd)
                        {
                            z.releaseStart = marks.releaseMarker;
                            // `ReleaseEnd` : fin de la queue (échantillons) ; `ReleaseCrossfadeLength` : fondu entre la boucle et la queue (ms)
                            const auto releaseEnd = ini_.getInt (section, variant.prefix + "ReleaseEnd", -1);
                            if (releaseEnd > z.releaseStart && releaseEnd > lastEnd) z.endFrame = releaseEnd;
                            z.releaseCrossfadeMs = std::clamp (ini_.getInt (section, variant.prefix + "ReleaseCrossfadeLength", 0), 0, 3000);
                        }
                    }
                }
                // `AttackStart` : la lecture saute ce début du fichier (échantillons)
                if (const auto attackStart = ini_.getInt (section, variant.prefix + "AttackStart", 0); attackStart > 0) z.startFrame = attackStart;
                members.push_back (kb.instrument.zones.size());
                kb.instrument.zones.push_back (std::move (z));
            }
            if (members.size() > 1)
                for (const std::size_t index : members)
                {
                    auto& z = kb.instrument.zones[index];
                    z.roundRobin = ++rank;
                    z.rrMode = index == members.front() ? static_cast<int> (RoundRobinMode::Random) : -1;
                }
        }
        if (ownRelease)
        {
            // durée maximale effective : sans aucune « sans limite », la plus longue en tient lieu (GrandOrgue se tairait au-delà ; un tuyau qui se tait d'un coup serait pire)
            int longest = -1;
            bool unbounded = false;
            for (const auto& r : releases) { if (r.maxMs < 0) unbounded = true; else longest = std::max (longest, r.maxMs); }
            for (auto& r : releases) if (! unbounded && r.maxMs == longest) r.maxMs = -1;
            std::vector<int> tops;
            for (const auto& r : releases) if (r.maxMs >= 0) tops.push_back (r.maxMs);
            std::sort (tops.begin(), tops.end());
            tops.erase (std::unique (tops.begin(), tops.end()), tops.end());
            if (std::any_of (releases.begin(), releases.end(), [] (const Release& r) { return r.maxMs < 0; })) tops.push_back (-1);
            int previous = -1;
            for (const int top : tops)
            {
                int rank = 0;
                std::vector<std::size_t> members;
                for (const auto& r : releases)
                {
                    if (r.maxMs != top) continue;
                    ZoneDescription z;
                    const fs::path path = resolvePath (opt_.baseDir, r.file);
                    z.samplePath = path.u8string();
                    z.keyLow = z.keyHigh = note;
                    z.rootNote = note;
                    z.tuneCents = tuneCents;
                    z.gainDb = gainTotal;
                    z.group = group;
                    z.mode = PlayMode::OneShot;
                    z.onRelease = true;
                    z.heldMinMs = previous < 0 ? 0 : previous + 1;
                    z.heldMaxMs = top < 0 ? 0 : std::max (top, 1);
                    const auto& marks = marksOf (path);
                    const std::int64_t cue = r.cue >= 0 ? r.cue : (marks.exists && marks.releaseMarker > 0 ? marks.releaseMarker : 0);
                    if (cue > 0) z.startFrame = cue;
                    if (r.end > z.startFrame) z.endFrame = r.end;   // `ReleaseEnd` : la lecture s'arrête là (échantillons)
                    if (r.crossfadeMs > 0) z.fadeInMs = std::max (z.fadeInMs, static_cast<double> (r.crossfadeMs));   // `ReleaseCrossfadeLength` : entrée en fondu du relâchement
                    members.push_back (kb.instrument.zones.size());
                    kb.instrument.zones.push_back (std::move (z));
                }
                if (members.size() > 1)
                    for (const std::size_t index : members)
                    {
                        auto& z = kb.instrument.zones[index];
                        z.roundRobin = ++rank;
                        z.rrMode = index == members.front() ? static_cast<int> (RoundRobinMode::Random) : -1;
                    }
                if (top >= 0) previous = top;
            }
        }
        ++kb.pipes;
    }

    // Le sommier d'un jeu : sa clef `WindchestGroup`, sinon celle de son premier rang.
    int windchestOf (const std::string& stopSection)
    {
        if (ini_.has (stopSection, "WindchestGroup")) return ini_.getInt (stopSection, "WindchestGroup", 0);
        if (ini_.getInt (stopSection, "NumberOfRanks", 0) > 0)
            return ini_.getInt (numbered ("Rank", ini_.getInt (stopSection, numbered ("Rank", 1), 0)), "WindchestGroup", 0);
        return 0;
    }

    // Les modulateurs que le sommier impose à ses jeux : ses boîtes expressives (volume réglable, niveau minimal) et ses tremblants (volume qui oscille tant que
    // leur interrupteur est tiré). Renvoie le masque à ajouter au Groupe du jeu ; -1 : plus de modulateur libre.
    bool windchestMask (std::size_t keyboard, Keyboard& kb, int windchest, ModMask& mask)
    {
        const std::string wc = numbered ("WindchestGroup", windchest);
        if (windchest <= 0 || ! ini_.hasSection (wc)) return true;
        const int enclosures = ini_.getInt (wc, "NumberOfEnclosures", 0);
        for (int j = 1; j <= enclosures; ++j)
        {
            const int number = ini_.getInt (wc, numbered ("Enclosure", j), 0);
            const std::string section = numbered ("Enclosure", number);
            if (number <= 0 || ! ini_.hasSection (section)) continue;
            const std::string key = "en" + std::to_string (number);
            const std::string name = ini_.get (section, "Name", section);
            const int index = modulatorFor (keyboard, key, "Box " + name);
            if (index < 0) return false;
            auto& m = kb.instrument.modulators[static_cast<std::size_t> (index)];
            m.continuous = true;
            m.floorLevel = std::clamp (ini_.getInt (section, "AmpMinimumLevel", 0) / 100.0, 0.0, 1.0);
            noteInitial (key, true);   // ouverte au départ
            if (boxKeys_.insert ({ keyboard, key }).second) boxControls_.push_back ({ keyboard, "Box " + name, index, key });
            ++boxUse_[{ keyboard, key }];
            usedEnclosures_.insert (number);
            mask |= static_cast<ModMask> (1u) << index;
        }
        const int tremulants = ini_.getInt (wc, "NumberOfTremulants", 0);
        for (int j = 1; j <= tremulants; ++j)
        {
            const int number = ini_.getInt (wc, numbered ("Tremulant", j), 0);
            const std::string section = numbered ("Tremulant", number);
            if (number <= 0 || ! ini_.hasSection (section)) continue;
            if (lowered (ini_.get (section, "TremulantType", "Synth")) == "wave")   // tremblant enregistré : ses échantillons ne sont pas lus, un tremblant de synthèse sonnerait autrement
            {
                waveTremulants_.insert (number);
                continue;
            }
            const std::string key = "tr" + std::to_string (number);
            const std::string name = ini_.get (section, "Name", section);
            // l'interrupteur du tremblant : le premier de sa liste, sinon le tremblant est son propre tirant
            std::string gateKey = "tm" + std::to_string (number);
            if (const int sw = ini_.getInt (section, "Switch001", 0); ini_.getInt (section, "SwitchCount", 0) > 0 && sw > 0)
            {
                gateKey = switchKey (sw);
                noteInitial (gateKey, ini_.getBool (numbered ("Switch", sw), "DefaultToEngaged", false));
            }
            else noteInitial (gateKey, ini_.getBool (section, "DefaultToEngaged", false));
            const int gate = modulatorFor (keyboard, gateKey, switchName (gateKey, name));
            const int index = modulatorFor (keyboard, key, "Vibrato " + name);
            if (gate < 0 || index < 0) return false;
            auto& m = kb.instrument.modulators[static_cast<std::size_t> (index)];
            m.continuous = true;
            m.tremPeriodMs = std::clamp (ini_.getDouble (section, "Period", 200.0), 20.0, 5000.0);
            m.tremDepth = std::clamp (ini_.getDouble (section, "AmpModDepth", 10.0) / 100.0, 0.0, 1.0);
            m.tremGate = gate;
            // vitesse : un modulateur de plus, à 50 % au départ (la période de l'orgue)
            const std::string speedKey = "ts" + std::to_string (number);
            const int speed = modulatorFor (keyboard, speedKey, "Speed " + name);
            if (speed < 0) return false;
            kb.instrument.modulators[static_cast<std::size_t> (speed)].continuous = true;
            m.tremSpeed = speed;
            levelOf_[speedKey] = 50.0;
            if (boxKeys_.insert ({ keyboard, speedKey }).second) boxControls_.push_back ({ keyboard, "Speed " + name, speed, speedKey });
            ++boxUse_[{ keyboard, speedKey }];
            noteInitial (key, true);
            levelOf_[key] = std::clamp (ini_.getDouble (section, "AmpModDepth", 10.0), 0.0, 100.0);   // le curseur de profondeur part de la valeur de l'orgue
            if (boxKeys_.insert ({ keyboard, key }).second) boxControls_.push_back ({ keyboard, "Depth " + name, index, key });
            ++boxUse_[{ keyboard, key }];
            usedTremulants_.insert (number);
            mask |= static_cast<ModMask> (1u) << index;
        }
        return true;
    }

    void buildStop (Keyboard& kb, std::size_t keyboard, const ManualInfo& info, int stopNumber)
    {
        const std::string section = numbered ("Stop", stopNumber);
        if (! ini_.hasSection (section)) { warn ("stop " + std::to_string (stopNumber) + " is listed but missing"); return; }
        const std::string name = ini_.get (section, "Name", section);
        std::vector<std::string> keys;
        const int count = ini_.getInt (section, "SwitchCount", 0);
        for (int j = 1; j <= count; ++j)
            if (const int sw = ini_.getInt (section, numbered ("Switch", j), 0); sw > 0) keys.push_back (switchKey (sw));
        const std::string own = "st" + std::to_string (stopNumber);
        if (keys.empty())
        {
            keys.push_back (own);   // un jeu sans interrupteur est son propre tirant
            noteInitial (own, ini_.getBool (section, "DefaultToEngaged", false));
        }
        bool known = true;
        combineOf (ini_.get (section, "Function"), known);
        if (! known || (count > 1 && lowered (ini_.get (section, "Function")) != "and" && ! ini_.get (section, "Function").empty()))
            warn ("stop \"" + name + "\": the switch function \"" + ini_.get (section, "Function") + "\" is treated as AND");
        ModMask mask = 0;
        for (const auto& key : keys)
        {
            if (key.rfind ("sw", 0) == 0) noteInitial (key, ini_.getBool (numbered ("Switch", std::atoi (key.c_str() + 2)), "DefaultToEngaged", false));
            const int index = modulatorFor (keyboard, key, switchName (key, name));
            if (index < 0) { warn ("keyboard \"" + kb.name + "\": more than " + std::to_string (kModulators) + " switches, stop \"" + name + "\" not imported"); return; }
            mask |= static_cast<ModMask> (1u) << index;
        }
        if (ini_.getInt (section, "NumberOfRanks", 0) == 0 && ! windchestMask (keyboard, kb, windchestOf (section), mask)) { warn ("keyboard \"" + kb.name + "\": more than " + std::to_string (kModulators) + " switches, stop \"" + name + "\" not imported"); return; }
        std::string group = name;
        for (int n = 2; usedGroups_[keyboard].count (group) != 0; ++n) group = name + " (" + std::to_string (n) + ")";
        usedGroups_[keyboard].insert (group);
        const std::size_t zonesBefore = kb.instrument.zones.size();
        const double stopAmp = ini_.getInt (section, "AmplitudeLevel", 100) / 100.0;
        const double stopTuning = ini_.getDouble (section, "PitchTuning", 0.0);
        const double stopGain = ini_.getDouble (section, "Gain", 0.0);
        const double base = organAmp_ / 100.0 * stopAmp;
        const int nRanks = ini_.getInt (section, "NumberOfRanks", 0);
        const int accessible = ini_.getInt (section, "NumberOfAccessiblePipes", ini_.getInt (section, "NumberOfLogicalPipes", 0));
        const int firstKey = ini_.getInt (section, "FirstAccessiblePipeLogicalKeyNumber", 1);
        bool leftOutByPerspective = false;
        if (nRanks > 0)
        {
            for (int r = 1; r <= nRanks; ++r)
            {
                const int rankNumber = ini_.getInt (section, numbered ("Rank", r), 0);
                const std::string rankSection = numbered ("Rank", rankNumber);
                if (! ini_.hasSection (rankSection)) { warn ("stop \"" + name + "\": rank " + std::to_string (rankNumber) + " is missing"); continue; }
                if (const auto prefix = rankPrefix (ini_.get (rankSection, "Name")); ! prefix.empty() && perspectives_.count (prefix) != 0 && excluded_.count (prefix) != 0)
                {
                    leftOutByPerspective = true;   // perspective non choisie : ce rang n'est pas chargé
                    continue;
                }
                const std::string prefix = numbered ("Rank", r);
                const int firstPipe = ini_.getInt (section, prefix + "FirstPipeNumber", 1);
                const int rankPipes = ini_.getInt (rankSection, "NumberOfLogicalPipes", 0);
                const int pipeCount = ini_.getInt (section, prefix + "PipeCount", std::min (accessible, rankPipes - firstPipe + 1));
                const int rankFirstKey = ini_.getInt (section, prefix + "FirstAccessibleKeyNumber", firstKey);
                const double rankAmp = ini_.getInt (rankSection, "AmplitudeLevel", 100) / 100.0;
                const double tuning = organTuning_ + stopTuning + ini_.getDouble (rankSection, "PitchTuning", 0.0);
                const double gain = organGain_ + stopGain + ini_.getDouble (rankSection, "Gain", 0.0);
                // le sommier d'un jeu à rangs est celui de chaque rang : ses boîtes et tremblants s'appliquent aux zones de ce rang
                ModMask rankMask = 0;
                if (! windchestMask (keyboard, kb, ini_.getInt (rankSection, "WindchestGroup", windchestOf (section)), rankMask))
                {
                    warn ("keyboard \"" + kb.name + "\": more than " + std::to_string (kModulators) + " switches, a rank of stop \"" + name + "\" has no box or tremulant");
                    rankMask = 0;
                }
                const std::size_t before = kb.instrument.zones.size();
                for (int j = 0; j < pipeCount; ++j)
                {
                    addPipe (kb, info, rankSection, firstPipe + j, rankFirstKey + j, group, base * rankAmp, tuning, gain);
                }
                if (rankMask != 0)
                    for (std::size_t z = before; z < kb.instrument.zones.size(); ++z) kb.instrument.zones[z].modulators |= rankMask;
            }
        }
        else
        {
            const int firstPipe = ini_.getInt (section, "FirstAccessiblePipeLogicalPipeNumber", 1);
            for (int j = 0; j < accessible; ++j)
            {
                addPipe (kb, info, section, firstPipe + j, firstKey + j, group, base, organTuning_ + stopTuning, organGain_ + stopGain);
            }
        }
        if (kb.instrument.zones.size() == zonesBefore)
        {
            if (leftOutByPerspective) ++stopsLeftOut_;
            else warn ("stop \"" + name + "\" has no playable pipe");
            return;
        }
        kb.instrument.groups[group].modulators = mask;
        ++kb.stops;
    }

    void buildKeyboard (int odfIndex, std::size_t keyboard)
    {
        const std::string section = numbered ("Manual", odfIndex);
        Keyboard kb;
        kb.odfIndex = odfIndex;
        kb.midiChannel = channelOf_[odfIndex];
        kb.name = ini_.get (section, "Name", odfIndex == 0 ? "Pedal" : "Manual " + std::to_string (odfIndex));
        kb.instrument.maxVoices = InstrumentDescription::maxVoicesLimit;
        kb.instrument.streamFromDisk = opt_.streamFromDisk;
        ManualInfo info;
        info.keyboard = keyboard;
        info.firstMidi = ini_.getInt (section, "FirstAccessibleKeyMIDINoteNumber", 36);
        info.firstLogical = ini_.getInt (section, "FirstAccessibleKeyLogicalKeyNumber", 1);
        const int logical = ini_.getInt (section, "NumberOfLogicalKeys", 61);
        info.accessible = ini_.getInt (section, "NumberOfAccessibleKeys", logical);
        manuals_[odfIndex] = info;
        if (! ini_.hasSection (section)) warn ("keyboard " + std::to_string (odfIndex) + " is missing from the file");
        result_.keyboards.push_back (std::move (kb));
        modulators_.emplace_back();
        usedGroups_.emplace_back();
        Keyboard& placed = result_.keyboards.back();
        const int stops = ini_.getInt (section, "NumberOfStops", 0);
        for (int i = 1; i <= stops; ++i)
            if (const int number = ini_.getInt (section, numbered ("Stop", i), 0); number > 0) buildStop (placed, keyboard, info, number);
    }

    void buildCouplers()
    {
        for (auto& kb : result_.keyboards)
        {
            const std::string section = numbered ("Manual", kb.odfIndex);
            const auto& info = manuals_[kb.odfIndex];
            const int count = ini_.getInt (section, "NumberOfCouplers", 0);
            for (int i = 1; i <= count; ++i)
            {
                const int number = ini_.getInt (section, numbered ("Coupler", i), 0);
                const std::string cs = numbered ("Coupler", number);
                if (number <= 0 || ! ini_.hasSection (cs)) continue;
                const std::string name = ini_.get (cs, "Name", cs);
                const int dest = ini_.getInt (cs, "DestinationManual", kb.odfIndex);
                const auto channel = channelOf_.find (dest);
                if (channel == channelOf_.end()) { warn ("coupler \"" + name + "\": destination keyboard " + std::to_string (dest) + " does not exist"); continue; }
                if (result_.router.rules.size() >= static_cast<std::size_t> (router::kMaxRules)) { warn ("too many couplers: \"" + name + "\" and the following ones are not imported"); break; }
                router::Rule rule;
                rule.name = name;
                rule.srcChannel = kb.midiChannel;
                rule.dstChannel = channel->second;
                rule.keyLo = std::clamp (info.firstMidi, 0, 127);
                rule.keyHi = std::clamp (info.firstMidi + info.accessible - 1, 0, 127);
                rule.transpose = std::clamp (ini_.getInt (cs, "DestinationKeyshift", 0), -48, 48);
                const std::string type = lowered (ini_.get (cs, "CouplerType", "normal"));
                rule.select = type == "bass" ? router::Select::Lowest : type == "melody" ? router::Select::Highest : router::Select::All;
                bool known = true;
                rule.combine = static_cast<router::Combine> (combineOf (ini_.get (cs, "Function"), known));
                if (! known) warn ("coupler \"" + name + "\": the switch function \"" + ini_.get (cs, "Function") + "\" is treated as AND");
                std::vector<std::string> keys;
                const int switches = ini_.getInt (cs, "SwitchCount", 0);
                for (int j = 1; j <= switches; ++j)
                    if (const int sw = ini_.getInt (cs, numbered ("Switch", j), 0); sw > 0) keys.push_back (switchKey (sw));
                if (keys.empty())
                {
                    keys.push_back ("cp" + std::to_string (number));
                    noteInitial (keys.back(), ini_.getBool (cs, "DefaultToEngaged", false));
                }
                for (const auto& key : keys)
                {
                    if (key.rfind ("sw", 0) == 0) noteInitial (key, ini_.getBool (numbered ("Switch", std::atoi (key.c_str() + 2)), "DefaultToEngaged", false));
                    if (rule.terms.size() >= static_cast<std::size_t> (router::kMaxTerms)) { warn ("coupler \"" + name + "\": more than " + std::to_string (router::kMaxTerms) + " switches, the extra ones are ignored"); break; }
                    if (const ParamId id = paramFor (key); id != 0)
                    {
                        rule.terms.push_back ({ id, false });
                        if (couplerControlKeys_.insert (key).second) couplerControls_.emplace_back (keys.size() == 1 ? name : switchName (key, name), id);
                    }
                    else warn ("coupler \"" + name + "\": no free router switch left for one of its switches");
                }
                result_.router.rules.push_back (std::move (rule));
            }
        }
    }

    // Mémoire de combinaison : tout interrupteur de `scope` (ou tous) y est tiré s'il est cité en positif, repoussé sinon.
    combi::Memory memoryOf (const std::string& name, const std::set<std::string>& on, const std::vector<std::string>* onlyKeys)
    {
        combi::Memory m;
        m.used = true;
        m.name = name.substr (0, combi::kMaxName);
        for (const auto& [key, ids] : params_)
        {
            if (onlyKeys != nullptr && std::find (onlyKeys->begin(), onlyKeys->end(), key) == onlyKeys->end()) continue;
            for (const ParamId id : ids)
                if (m.values.size() < combi::kMaxValues) m.values.emplace_back (id, valueFor (id, on.count (key) != 0));
        }
        return m;
    }

    void buildCombinations()
    {
        // généraux : tout l'état de la console
        const int generals = ini_.getInt ("Organ", "NumberOfGenerals", 0);
        for (int g = 1; g <= generals; ++g)
        {
            const std::string section = numbered ("General", g);
            if (! ini_.hasSection (section)) continue;
            const int index = g - 1;
            if (index >= combi::kBanks * combi::kPistons) { warn ("more than " + std::to_string (combi::kBanks * combi::kPistons) + " generals: the extra ones are not imported"); break; }
            std::set<std::string> on;
            const int switches = ini_.getInt (section, "NumberOfSwitches", 0);
            for (int i = 1; i <= switches; ++i)
                if (const int n = ini_.getInt (section, numbered ("SwitchNumber", i), 0); n > 0) on.insert (switchKey (n));
            const int stops = ini_.getInt (section, "NumberOfStops", 0);
            for (int i = 1; i <= stops; ++i)
                if (const int n = ini_.getInt (section, numbered ("StopNumber", i), 0); n > 0) on.insert ("st" + std::to_string (n));
            result_.combinations.generals[combi::generalKey (index / combi::kPistons, index % combi::kPistons)] = memoryOf (ini_.get (section, "Name", std::to_string (g)), on, nullptr);
        }
        // divisionnels : seulement les interrupteurs du clavier
        for (std::size_t k = 0; k < result_.keyboards.size() && opt_.firstTrack + static_cast<int> (k) < combi::kTracks; ++k)
        {
            const auto& kb = result_.keyboards[k];
            const std::string manual = numbered ("Manual", kb.odfIndex);
            const int count = ini_.getInt (manual, "NumberOfDivisionals", 0);
            std::vector<std::string> own;
            for (const auto& [key, index] : modulators_[k]) own.push_back (key);
            for (int d = 1; d <= count; ++d)
            {
                const std::string section = manual + numbered ("Divisional", d);
                if (! ini_.hasSection (section)) continue;
                const int index = d - 1;
                if (index >= combi::kBanks * combi::kPistons) break;
                std::set<std::string> on;
                const int switches = ini_.getInt (section, "NumberOfSwitches", 0);
                for (int i = 1; i <= switches; ++i)
                    if (const int n = ini_.getInt (section, numbered ("Switch", i), 0); n > 0) on.insert (switchKey (n));
                const int stops = ini_.getInt (section, "NumberOfStops", 0);
                for (int i = 1; i <= stops; ++i)
                    if (const int n = ini_.getInt (section, numbered ("Stop", i), 0); n > 0) on.insert ("st" + std::to_string (n));
                auto memory = memoryOf (ini_.get (section, "Name", std::to_string (d)), on, &own);
                // les valeurs d'un divisionnel ne visent que les modulateurs de ce clavier
                memory.values.erase (std::remove_if (memory.values.begin(), memory.values.end(), [&] (const auto& v)
                {
                    for (int i = 0; i < kModulators; ++i)
                        if (v.first == mixer::modulatorParam (opt_.firstTrack + static_cast<int> (k), i)) return false;
                    return true;
                }), memory.values.end());
                result_.combinations.divisionals[combi::divisionalKey (opt_.firstTrack + static_cast<int> (k), index / combi::kPistons, index % combi::kPistons)] = std::move (memory);
            }
        }
    }

    void otherThings()
    {
        const int tremulants = ini_.getInt ("Organ", "NumberOfTremulants", 0);
        for (int t = 1; t <= tremulants; ++t)
            if (waveTremulants_.count (t) != 0) warn ("tremulant \"" + ini_.get (numbered ("Tremulant", t), "Name", numbered ("Tremulant", t)) + "\" is recorded (wave type): not imported");
            else if (usedTremulants_.count (t) == 0) warn ("tremulant \"" + ini_.get (numbered ("Tremulant", t), "Name", numbered ("Tremulant", t)) + "\" acts on no stop: not imported");
        const int enclosures = ini_.getInt ("Organ", "NumberOfEnclosures", 0);
        for (int e = 1; e <= enclosures; ++e)
            if (usedEnclosures_.count (e) == 0) warn ("expression box \"" + ini_.get (numbered ("Enclosure", e), "Name", numbered ("Enclosure", e)) + "\" acts on no stop: not imported");
        if (repeatAttacksSkipped_ > 0) warn ("attack variants for a pipe that has just been released (MaxTimeSinceLastRelease) are not imported: " + std::to_string (repeatAttacksSkipped_) + " skipped");
        if (ini_.getInt ("Organ", "NumberOfReversiblePistons", 0) > 0) warn ("reversible pistons are not imported");
        if (ini_.getInt ("Organ", "NumberOfDivisionalCouplers", 0) > 0) warn ("divisional couplers are not imported");
    }

    const IniFile& ini_;
    Options opt_;
    std::set<std::string> perspectives_;   // préfixes de rangs qui sont des perspectives (voir perspectivePrefixes)
    std::set<std::string> excluded_;       // perspectives dont les rangs ne sont pas chargés
    int stopsLeftOut_ = 0;                 // jeux sans aucun rang dans les perspectives choisies
    Result result_;
    struct BoxControl { std::size_t keyboard; std::string name; int index; std::string key; };
    std::vector<BoxControl> boxControls_;
    std::set<std::pair<std::size_t, std::string>> boxKeys_;
    std::map<std::pair<std::size_t, std::string>, int> boxUse_;   // jeux de ce clavier qui dépendent de cette boîte
    std::set<int> usedEnclosures_, usedTremulants_, waveTremulants_;
    std::map<std::string, double> levelOf_;   // valeur de départ d'un réglage continu (profondeur d'un tremblant)
    std::set<std::string> couplerControlKeys_;
    std::vector<std::pair<std::string, ParamId>> couplerControls_;
    int repeatAttacksSkipped_ = 0;
    int organAmp_ = 100;
    double organTuning_ = 0.0, organGain_ = 0.0;
    std::map<int, int> channelOf_;
    std::map<int, ManualInfo> manuals_;
    std::vector<std::map<std::string, int>> modulators_;     // par clavier : interrupteur -> indice de modulateur
    std::vector<std::set<std::string>> usedGroups_;
    std::map<std::string, std::vector<ParamId>> params_;     // interrupteur -> ses paramètres (un par clavier qui le porte, ou un tirant du routeur)
    std::map<std::string, bool> initial_;
    std::map<std::string, FileMarks> marks_;
    int routerSwitches_ = 0;
};
} // namespace

std::vector<Perspective> listPerspectives (std::string_view odfText)
{
    const IniFile ini = IniFile::parse (odfText);
    const auto prefixes = perspectivePrefixes (ini);
    std::vector<Perspective> out;
    if (prefixes.size() < 2) return out;
    std::map<std::string, std::size_t> where;
    for (const auto& section : ini.sections())
    {
        const std::string low = lowered (section);
        if (low.size() < 5 || low.rfind ("rank", 0) != 0 || low.find_first_not_of ("0123456789", 4) != std::string::npos) continue;
        const std::string prefix = rankPrefix (ini.get (section, "Name"));
        if (prefixes.count (prefix) == 0) continue;
        auto it = where.find (prefix);
        if (it == where.end()) { it = where.emplace (prefix, out.size()).first; out.push_back ({ prefix, 0, 0 }); }
        ++out[it->second].ranks;
        out[it->second].pipes += ini.getInt (section, "NumberOfLogicalPipes", 0);
    }
    return out;
}

std::vector<Perspective> listPerspectivesFile (const fs::path& odfFile)
{
    std::ifstream in (odfFile, std::ios::binary);
    if (! in) return {};
    const std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    return listPerspectives (text);
}

Result importOrgan (std::string_view odfText, const Options& options)
{
    const IniFile ini = IniFile::parse (odfText);
    return Builder (ini, options).run();
}

Result importOrganFile (const fs::path& odfFile, Options options)
{
    std::ifstream in (odfFile, std::ios::binary);
    if (! in)
    {
        Result r;
        r.error = "cannot read " + odfFile.u8string();
        return r;
    }
    const std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (options.baseDir.empty()) options.baseDir = odfFile.parent_path();
    return importOrgan (text, options);
}
} // namespace neptune::core::organimport
