#include "neptune/core/Facade.h"
#include "neptune/core/Combinations.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace neptune::core::facade
{
using nlohmann::json;

int Facade::freeId() const noexcept
{
    int id = 1;
    for (const auto& w : widgets) id = std::max (id, w.id + 1);
    return id;
}

const std::vector<ImageRole>& imageRoles()
{
    static const std::vector<ImageRole> roles = {
        { "off", &ImageStyle::off }, { "on", &ImageStyle::on }, { "off_hover", &ImageStyle::offHover }, { "on_hover", &ImageStyle::onHover },
        { "disabled", &ImageStyle::disabled }, { "track", &ImageStyle::track }, { "handle", &ImageStyle::handle }, { "strip", &ImageStyle::strip },
        { "body", &ImageStyle::body }, { "pointer", &ImageStyle::pointer }, { "key_white", &ImageStyle::keyWhite },
        { "key_white_on", &ImageStyle::keyWhiteOn }, { "key_black", &ImageStyle::keyBlack }, { "key_black_on", &ImageStyle::keyBlackOn },
        { "frame", &ImageStyle::frame } };
    return roles;
}

int Facade::freeStyleId() const noexcept
{
    int id = 1;
    for (const auto& s : styles) id = std::max (id, s.id + 1);
    return id;
}

const ImageStyle* Facade::findStyle (int id) const noexcept
{
    for (const auto& s : styles)
        if (s.id == id) return &s;
    return nullptr;
}

const Widget* Facade::find (int id) const noexcept
{
    for (const auto& w : widgets)
        if (w.id == id) return &w;
    return nullptr;
}

Widget* Facade::find (int id) noexcept
{
    for (auto& w : widgets)
        if (w.id == id) return &w;
    return nullptr;
}

double notchPosition (const std::vector<double>& notches, double plain) noexcept
{
    if (notches.size() < 2) return 0.0;
    std::size_t best = 0;
    for (std::size_t i = 1; i < notches.size(); ++i)
        if (std::abs (notches[i] - plain) < std::abs (notches[best] - plain)) best = i;
    return static_cast<double> (best) / static_cast<double> (notches.size() - 1);
}

double notchValueAt (const std::vector<double>& notches, double position) noexcept
{
    if (notches.empty()) return 0.0;
    if (notches.size() == 1) return notches.front();
    const double t = std::clamp (std::isfinite (position) ? position : 0.0, 0.0, 1.0);
    return notches[static_cast<std::size_t> (std::lround (t * static_cast<double> (notches.size() - 1)))];
}

std::vector<double> evenNotches (const ParamSpec& spec, int n)
{
    n = std::clamp (n, 2, static_cast<int> (maxNotches));
    std::vector<double> out;
    for (int i = 0; i < n; ++i)
    {
        const double v = spec.fromNormalized (static_cast<double> (i) / static_cast<double> (n - 1));
        if (out.empty() || std::abs (v - out.back()) > 1.0e-9) out.push_back (v);   // un paramètre à pas peut avoir moins de valeurs que demandé
    }
    return out;
}

Widget defaultWidget (WidgetType type, int id, int x, int y)
{
    Widget w;
    w.id = id;
    w.type = type;
    switch (type)
    {
        case WidgetType::Knob:   w.w = 90;  w.h = 110; break;
        case WidgetType::Button: w.w = 120; w.h = 44;  break;
        case WidgetType::Label:  w.w = 160; w.h = 32;  break;
        case WidgetType::Menu:   w.w = 170; w.h = 56;  break;
        case WidgetType::Action: w.w = 130; w.h = 44;  break;
        case WidgetType::Curve:  w.w = 340; w.h = 260; break;
        case WidgetType::Zone:   w.w = 320; w.h = 200; break;
        case WidgetType::Keyboard: w.w = 520; w.h = 110; break;
        case WidgetType::Slider: w.w = 260; w.h = 44; break;
        case WidgetType::Lamp:   w.w = 40;  w.h = 40;  break;
        case WidgetType::Set:    w.w = 90;  w.h = 44;  break;
        case WidgetType::Midi:   w.w = 150; w.h = 44;  break;
        case WidgetType::Meter:  w.w = 44;  w.h = 180; w.vertical = 1; break;
        case WidgetType::SceneGrid: w.w = 640; w.h = 400; w.columns = 4; w.rows = 4; break;
    }
    w.x = x;
    w.y = y;
    return w;
}

namespace
{
std::string clipped (std::string s, std::size_t maxLength)
{
    if (s.size() > maxLength) s.resize (maxLength);
    return s;
}
} // namespace

namespace
{
constexpr int kMeterTracks = 16, kMeterBuses = 16, kMeterParallels = 32;   // = mixer::numTracks / numBuses / numParallels (Facade n'inclut pas Mixer.h)
}

namespace
{
// Valeur réelle pour une progression u (0..1) de la course logarithmique entre min et max (min <= 0 : décalée pour rester positive).
double logValue (const ParamSpec& s, double u) noexcept
{
    if (s.min > 0.0) return s.min * std::pow (s.max / s.min, u);
    const double c = (s.max - s.min) * 0.001;
    return s.min - c + c * std::pow ((s.max - s.min + c) / c, u);
}

double logProgress (const ParamSpec& s, double plain) noexcept
{
    if (s.min > 0.0) return std::log (std::max (plain, s.min) / s.min) / std::log (s.max / s.min);
    const double c = (s.max - s.min) * 0.001;
    return std::log (std::max (plain - s.min + c, c) / c) / std::log ((s.max - s.min + c) / c);
}

bool scalable (const Widget& w, const ParamSpec& s) noexcept
{
    return s.kind == Kind::Continuous && s.max > s.min && (w.scale != 0 || w.response.active());
}
} // namespace

double widgetNormalised (const Widget& w, const ParamSpec& s, double position) noexcept
{
    double t = std::clamp (position, 0.0, 1.0);
    if (! scalable (w, s)) return t;
    if (w.response.active()) t = evaluate (w.response, t);
    switch (w.scale)
    {
        case 1: return s.toNormalized (s.min + t * (s.max - s.min));
        case 2: return s.toNormalized (logValue (s, t));
        case 3: return s.toNormalized (s.min + s.max - logValue (s, 1.0 - t));
        default: return t;
    }
}

double widgetPosition (const Widget& w, const ParamSpec& s, double normalised) noexcept
{
    const double n = std::clamp (normalised, 0.0, 1.0);
    if (! scalable (w, s)) return n;
    const double plain = s.fromNormalized (n);
    double t = n;
    switch (w.scale)
    {
        case 1: t = (plain - s.min) / (s.max - s.min); break;
        case 2: t = logProgress (s, plain); break;
        case 3: t = 1.0 - logProgress (s, s.min + s.max - plain); break;
        default: break;
    }
    t = std::clamp (t, 0.0, 1.0);
    if (! w.response.active()) return t;
    // réciproque de la courbe libre (croissante) par dichotomie
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (evaluate (w.response, mid) < t) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

MeterSource decodeMeterSource (int source) noexcept
{
    MeterSource m;
    if (source == 1) { m.kind = MeterSource::Kind::Master; m.index = 0; return m; }
    if (source == 2) { m.reduction = true; return m; }
    const int family = source / 100, i = source % 100;
    switch (family)
    {
        case 1: m.kind = MeterSource::Kind::Track; m.index = i; break;
        case 2: m.kind = MeterSource::Kind::Bus; m.index = i; break;
        case 3: m.kind = MeterSource::Kind::Parallel; m.index = i; break;
        case 4: m.kind = MeterSource::Kind::Track; m.index = i; m.reduction = true; break;
        case 5: m.kind = MeterSource::Kind::Bus; m.index = i; m.reduction = true; break;
        case 6: m.kind = MeterSource::Kind::Parallel; m.index = i; m.reduction = true; break;
        case 7: m.kind = MeterSource::Kind::Master; m.index = 0; m.reduction = true; break;
        default: break;
    }
    return m;
}

int encodeMeterSource (const MeterSource& m) noexcept
{
    if (m.kind == MeterSource::Kind::Master) return m.reduction ? 700 : 1;
    if (m.index < 0) return m.reduction ? 2 : 0;
    const int base = m.kind == MeterSource::Kind::Track ? 100 : m.kind == MeterSource::Kind::Bus ? 200 : 300;
    return base + (m.reduction ? 300 : 0) + m.index;
}

int sanitisedMeterSource (int source) noexcept
{
    if (source >= 0 && source <= 2) return source;
    const auto m = decodeMeterSource (source);
    const int limit = m.kind == MeterSource::Kind::Track ? kMeterTracks : m.kind == MeterSource::Kind::Bus ? kMeterBuses : kMeterParallels;
    if (m.kind == MeterSource::Kind::Master) return source == 700 ? 700 : 0;
    if (m.index < 0 || m.index >= limit) return 0;
    return encodeMeterSource (m) == source ? source : 0;
}

namespace
{
Facade sanitisedPage (Facade f)
{
    f.image = clipped (std::move (f.image), maxImagePath);
    if (f.styles.size() > static_cast<std::size_t> (maxStyles)) f.styles.resize (maxStyles);
    {
        std::set<int> usedStyles;
        for (auto& st : f.styles)
        {
            st.name = clipped (std::move (st.name), maxTextLength);
            for (const auto& role : imageRoles()) st.*(role.field) = clipped (std::move (st.*(role.field)), maxImagePath);
            st.frames = std::clamp (st.frames, 0, maxStripFrames);
            st.angleMin = std::clamp (st.angleMin, -360, 360);
            st.angleMax = std::clamp (st.angleMax, -360, 360);
            st.pivotX = std::clamp (st.pivotX, 0, 100);
            st.pivotY = std::clamp (st.pivotY, 0, 100);
            st.sliceLeft = std::clamp (st.sliceLeft, 0, 45);
            st.sliceTop = std::clamp (st.sliceTop, 0, 45);
            st.sliceRight = std::clamp (st.sliceRight, 0, 45);
            st.sliceBottom = std::clamp (st.sliceBottom, 0, 45);
            st.sliceScale = std::clamp (st.sliceScale, 25, 400);
            st.textSize = std::clamp (st.textSize, 0, 100);
            st.textPosition = std::clamp (st.textPosition, 0, 4);
            st.textColour = clipped (std::move (st.textColour), 16);
            st.keyLit = clipped (std::move (st.keyLit), 16);
            if (st.id <= 0 || usedStyles.count (st.id) != 0)
            {
                int id = 1;
                while (usedStyles.count (id) != 0) ++id;
                st.id = id;
            }
            usedStyles.insert (st.id);
        }
    }
    const int fit = static_cast<int> (f.fit);
    if (fit < 0 || fit >= numImageFits) f.fit = ImageFit::Fit;
    if (f.widgets.size() > static_cast<std::size_t> (maxWidgets)) f.widgets.resize (maxWidgets);
    std::set<int> used;
    for (auto& w : f.widgets)
    {
        const int type = static_cast<int> (w.type);
        if (type < 0 || type >= numWidgetTypes) w.type = WidgetType::Knob;
        w.w = std::clamp (w.w, minWidgetSize, canvasWidth);
        w.h = std::clamp (w.h, minWidgetSize, canvasHeight);
        w.x = std::clamp (w.x, 0, canvasWidth - w.w);
        w.y = std::clamp (w.y, 0, canvasHeight - w.h);
        w.text = clipped (std::move (w.text), maxTextLength);
        if (w.snapshot.size() > maxSnapshot) w.snapshot.resize (maxSnapshot);
        w.snapshot.erase (std::remove_if (w.snapshot.begin(), w.snapshot.end(), [] (const auto& e) { return e.first == 0 || ! std::isfinite (e.second); }), w.snapshot.end());
        w.layout = std::clamp (w.layout, 0, numZoneLayouts - 1);
        w.align = std::clamp (w.align, 0, 2);
        w.justify = std::clamp (w.justify, 0, 3);
        w.gap = std::clamp (w.gap, 0, maxZoneGap);
        w.columns = w.type == WidgetType::SceneGrid ? std::clamp (w.columns, 4, 8) : std::clamp (w.columns, 1, maxZoneColumns);
        w.rows = std::clamp (w.rows, 4, 8);
        w.hue = ((w.hue % 360) + 360) % 360;
        w.vivid = std::clamp (w.vivid, 0, 100);
        w.header = w.header != 0 ? 1 : 0;
        w.channel = std::clamp (w.channel, 1, 16);
        w.firstNote = std::clamp (w.firstNote, 0, 120);
        w.hold = w.hold != 0 ? 1 : 0;
        w.velocity = std::clamp (w.velocity, 1, 127);
        w.wheels = w.wheels != 0 ? 1 : 0;
        w.vertical = w.vertical != 0 ? 1 : 0;
        w.source = w.type == WidgetType::Meter ? sanitisedMeterSource (w.source) : std::clamp (w.source, 0, 2);
        w.actionMode = std::clamp (w.actionMode, 0, 1 + combi::kNumCommands);
        if (w.mirror.size() > 16) w.mirror.resize (16);
        w.midi = midisend::sanitised (std::move (w.midi));
        w.midiTarget = std::clamp (w.midiTarget, 1, 3);
        w.midiFader = w.midiFader != 0 ? 1 : 0;
        w.invert = w.invert != 0 ? 1 : 0;
        w.scale = std::clamp (w.scale, 0, numWidgetScales - 1);
        w.response = sanitised (std::move (w.response));
        w.stepMode = std::clamp (w.stepMode, 0, 2);
        w.notches.erase (std::remove_if (w.notches.begin(), w.notches.end(), [] (double v) { return ! std::isfinite (v); }), w.notches.end());
        std::sort (w.notches.begin(), w.notches.end());
        w.notches.erase (std::unique (w.notches.begin(), w.notches.end()), w.notches.end());
        if (w.notches.size() > maxNotches) w.notches.resize (maxNotches);
        if (w.stepMode == 2 && w.notches.size() < 2) w.stepMode = 0;   // il faut au moins deux crans
        if (w.id <= 0 || used.count (w.id) != 0)
        {
            int id = 1;
            while (used.count (id) != 0) ++id;
            w.id = id;
        }
        used.insert (w.id);
    }
    for (auto& w : f.widgets)
        if (w.style != 0 && f.findStyle (w.style) == nullptr) w.style = 0;   // style supprimé
    // rattachements valides : la zone existe, n'est pas le widget lui-même, et la chaîne de parents ne boucle pas
    for (auto& w : f.widgets)
    {
        const auto* z = w.parent != 0 && w.parent != w.id ? f.find (w.parent) : nullptr;
        if (z == nullptr || z->type != WidgetType::Zone) w.parent = 0;
    }
    for (auto& w : f.widgets)
    {
        int steps = 0;
        for (int p = w.parent; p != 0; p = f.find (p)->parent)
            if (p == w.id || ++steps > 8) { w.parent = 0; break; }
    }
    return f;
}
} // namespace

namespace
{
std::vector<Widget*> pick (Facade& f, const std::set<int>& ids)
{
    std::vector<Widget*> out;
    for (auto& w : f.widgets)
        if (ids.count (w.id) != 0) out.push_back (&w);
    return out;
}
void keepInside (Widget& w)
{
    w.x = std::clamp (w.x, 0, canvasWidth - w.w);
    w.y = std::clamp (w.y, 0, canvasHeight - w.h);
}
} // namespace

void alignWidgets (Facade& f, const std::set<int>& ids, Align a)
{
    auto ws = pick (f, ids);
    if (ws.size() < 2) return;
    int lo = canvasWidth, hi = 0, top = canvasHeight, bottom = 0;
    for (const auto* w : ws) { lo = std::min (lo, w->x); hi = std::max (hi, w->x + w->w); top = std::min (top, w->y); bottom = std::max (bottom, w->y + w->h); }
    for (auto* w : ws)
    {
        switch (a)
        {
            case Align::Left:    w->x = lo; break;
            case Align::Right:   w->x = hi - w->w; break;
            case Align::Top:     w->y = top; break;
            case Align::Bottom:  w->y = bottom - w->h; break;
            case Align::CentreH: w->x = (lo + hi - w->w) / 2; break;
            case Align::CentreV: w->y = (top + bottom - w->h) / 2; break;
        }
        keepInside (*w);
    }
}

void distributeWidgets (Facade& f, const std::set<int>& ids, bool horizontal)
{
    auto ws = pick (f, ids);
    if (ws.size() < 3) return;
    std::sort (ws.begin(), ws.end(), [horizontal] (const Widget* a, const Widget* b) { return horizontal ? a->x < b->x : a->y < b->y; });
    int total = 0;
    for (const auto* w : ws) total += horizontal ? w->w : w->h;
    const int first = horizontal ? ws.front()->x : ws.front()->y;
    const int last = horizontal ? ws.back()->x + ws.back()->w : ws.back()->y + ws.back()->h;
    const int gap = (last - first - total) / static_cast<int> (ws.size() - 1);
    int pos = first;
    for (auto* w : ws)
    {
        (horizontal ? w->x : w->y) = pos;
        pos += (horizontal ? w->w : w->h) + gap;
        keepInside (*w);
    }
}

void arrangeGrid (Facade& f, const std::set<int>& ids, int columns)
{
    auto ws = pick (f, ids);
    if (ws.size() < 2 || columns < 1) return;
    columns = std::min (columns, static_cast<int> (ws.size()));
    int x0 = canvasWidth, y0 = canvasHeight, cellW = 0, cellH = 0;
    for (const auto* w : ws) { x0 = std::min (x0, w->x); y0 = std::min (y0, w->y); cellW = std::max (cellW, w->w); cellH = std::max (cellH, w->h); }
    const int gapX = 12, gapY = 12;
    for (std::size_t i = 0; i < ws.size(); ++i)
    {
        const int c = static_cast<int> (i) % columns, r = static_cast<int> (i) / columns;
        ws[i]->x = x0 + c * (cellW + gapX);
        ws[i]->y = y0 + r * (cellH + gapY);
        keepInside (*ws[i]);
    }
}

// ---- Zones ----

std::set<int> descendants (const Facade& f, int id)
{
    std::set<int> out;
    bool grew = true;
    while (grew)
    {
        grew = false;
        for (const auto& w : f.widgets)
            if (w.parent != 0 && (w.parent == id || out.count (w.parent) != 0) && out.insert (w.id).second) grew = true;
    }
    return out;
}

namespace
{
void reassign (Facade& f, Widget& w)
{
    const auto below = descendants (f, w.id);
    const Widget* best = nullptr;
    const int cx = w.x + w.w / 2, cy = w.y + w.h / 2;
    for (const auto& z : f.widgets)
    {
        if (z.type != WidgetType::Zone || z.id == w.id || below.count (z.id) != 0) continue;
        if (cx < z.x || cx >= z.x + z.w || cy < z.y || cy >= z.y + z.h) continue;
        if (z.w * z.h <= w.w * w.h && w.type == WidgetType::Zone) continue;   // une zone n'entre que dans une zone plus grande
        if (best == nullptr || z.w * z.h < best->w * best->h) best = &z;
    }
    w.parent = best != nullptr ? best->id : 0;
}
} // namespace

void assignParents (Facade& f, const std::set<int>& touched)
{
    std::set<int> zonesTouched;
    for (auto& w : f.widgets)
        if (touched.count (w.id) != 0)
        {
            reassign (f, w);
            if (w.type == WidgetType::Zone) zonesTouched.insert (w.id);
        }
    // une zone qui a bougé ou grandi adopte les widgets libres qu'elle couvre maintenant, et lâche ceux qui sont sortis
    for (const int zid : zonesTouched)
        for (auto& w : f.widgets)
            if (touched.count (w.id) == 0 && (w.parent == 0 || w.parent == zid)) reassign (f, w);
}

void moveWidgets (Facade& f, const std::set<int>& ids, int dx, int dy)
{
    std::set<int> all = ids;
    for (const int id : ids) all.merge (descendants (f, id));
    for (auto& w : f.widgets)
        if (all.count (w.id) != 0) { w.x += dx; w.y += dy; }
}

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Angle d'écran (y vers le bas : le sens croissant est le sens horaire) de la place de départ.
double startAngle (CircleStart s) noexcept
{
    switch (s)
    {
        case CircleStart::Top:    return -kPi / 2.0;
        case CircleStart::Right:  return 0.0;
        case CircleStart::Bottom: return kPi / 2.0;
        default:                  return kPi;
    }
}

// Écart horaire de `angle` depuis `from`, dans [0, 2π[ (les erreurs d'arrondi près de 2π comptent pour 0).
double clockwiseFrom (double angle, double from) noexcept
{
    double d = std::fmod (angle - from, 2.0 * kPi);
    if (d < 0.0) d += 2.0 * kPi;
    return d > 2.0 * kPi - 0.02 ? 0.0 : d;
}

void centreOn (Facade& f, int id, double px, double py)
{
    const auto& w = *f.find (id);
    const int nx = std::clamp (static_cast<int> (std::lround (px - w.w / 2.0)), 0, canvasWidth - w.w);
    const int ny = std::clamp (static_cast<int> (std::lround (py - w.h / 2.0)), 0, canvasHeight - w.h);
    moveWidgets (f, { id }, nx - w.x, ny - w.y);
}

// `order[0]` à l'angle `theta0`, les suivants à pas égaux dans le sens horaire.
void spreadOnCircle (Facade& f, const std::vector<int>& order, double cx, double cy, double r, double theta0)
{
    const double step = 2.0 * kPi / static_cast<double> (order.size());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        const double a = theta0 + step * static_cast<double> (i);
        centreOn (f, order[i], cx + r * std::cos (a), cy + r * std::sin (a));
    }
}
} // namespace

void arrangeCircle (Facade& f, const std::set<int>& ids, int refId, CircleStart start)
{
    auto ws = pick (f, ids);
    if (ws.size() < 3) return;
    int lo = canvasWidth, hi = 0, top = canvasHeight, bottom = 0, maxW = 0, maxH = 0;
    for (const auto* w : ws) { lo = std::min (lo, w->x); hi = std::max (hi, w->x + w->w); top = std::min (top, w->y); bottom = std::max (bottom, w->y + w->h); maxW = std::max (maxW, w->w); maxH = std::max (maxH, w->h); }
    const double cx = (lo + hi) / 2.0, cy = (top + bottom) / 2.0;
    const double n = static_cast<double> (ws.size());
    double r = std::min (hi - lo - maxW, bottom - top - maxH) / 2.0;
    r = std::max ({ r, 40.0, n * std::max (maxW, maxH) / (2.0 * kPi) });   // jamais trop serré pour que les widgets se chevauchent
    const Widget* ref = ids.count (refId) != 0 ? f.find (refId) : ws.front();
    const double refAngle = std::atan2 (ref->y + ref->h / 2.0 - cy, ref->x + ref->w / 2.0 - cx);
    std::vector<int> others;
    for (const auto* w : ws)
        if (w->id != ref->id) others.push_back (w->id);
    std::stable_sort (others.begin(), others.end(), [&] (int a, int b)
    {
        const auto &wa = *f.find (a), &wb = *f.find (b);
        return clockwiseFrom (std::atan2 (wa.y + wa.h / 2.0 - cy, wa.x + wa.w / 2.0 - cx), refAngle)
             < clockwiseFrom (std::atan2 (wb.y + wb.h / 2.0 - cy, wb.x + wb.w / 2.0 - cx), refAngle);
    });
    std::vector<int> order { ref->id };
    order.insert (order.end(), others.begin(), others.end());
    spreadOnCircle (f, order, cx, cy, r, startAngle (start));
}

void applyLayouts (Facade& f)
{
    // zones les moins profondes d'abord : une zone rangée par sa parente emporte son contenu avant d'être rangée à son tour
    auto depth = [&f] (const Widget& w)
    {
        int d = 0;
        for (int p = w.parent; p != 0 && d < 16; p = f.find (p)->parent) ++d;
        return d;
    };
    std::vector<int> zones;
    for (const auto& w : f.widgets)
        if (w.type == WidgetType::Zone && w.layout != static_cast<int> (ZoneLayout::Free)) zones.push_back (w.id);
    std::stable_sort (zones.begin(), zones.end(), [&] (int a, int b) { return depth (*f.find (a)) < depth (*f.find (b)); });

    for (const int zid : zones)
    {
        const Widget z = *f.find (zid);
        std::vector<int> kids;
        for (const auto& w : f.widgets)
            if (w.parent == zid) kids.push_back (w.id);
        if (kids.empty()) continue;
        const int cx = z.x + zonePad, cy = z.y + zonePad + (z.text.empty() ? 0 : zoneTitleHeight);
        const int cw = std::max (1, z.w - 2 * zonePad), ch = std::max (1, z.h - 2 * zonePad - (z.text.empty() ? 0 : zoneTitleHeight));
        const auto layout = static_cast<ZoneLayout> (z.layout);
        // ordre de rangement : par position (ligne : de gauche à droite ; colonne : de haut en bas ; matrice : ligne par ligne)
        const double midX = cx + cw / 2.0, midY = cy + ch / 2.0;
        const double theta0 = startAngle (static_cast<CircleStart> (z.justify));
        std::stable_sort (kids.begin(), kids.end(), [&] (int a, int b)
        {
            const auto &wa = *f.find (a), &wb = *f.find (b);
            if (layout == ZoneLayout::Circle)
                return clockwiseFrom (std::atan2 (wa.y + wa.h / 2.0 - midY, wa.x + wa.w / 2.0 - midX), theta0)
                     < clockwiseFrom (std::atan2 (wb.y + wb.h / 2.0 - midY, wb.x + wb.w / 2.0 - midX), theta0);
            if (layout == ZoneLayout::Row) return wa.x < wb.x;
            if (layout == ZoneLayout::Column) return wa.y < wb.y;
            const int ra = (wa.y + wa.h / 2) / 40, rb = (wb.y + wb.h / 2) / 40;   // même rangée à 40 px près
            return ra != rb ? ra < rb : wa.x < wb.x;
        });
        auto place = [&] (int id, int x, int y)
        {
            auto& w = *f.find (id);
            const int nx = std::clamp (x, 0, canvasWidth - w.w), ny = std::clamp (y, 0, canvasHeight - w.h);
            moveWidgets (f, { id }, nx - w.x, ny - w.y);
        };
        auto cross = [&] (int start, int size, int itemSize)
        {
            switch (static_cast<CrossAlign> (z.align))
            {
                case CrossAlign::Centre: return start + (size - itemSize) / 2;
                case CrossAlign::End:    return start + size - itemSize;
                default:                 return start;
            }
        };
        auto mainStart = [&] (int start, int size, int total)
        {
            switch (static_cast<Justify> (z.justify))
            {
                case Justify::Centre: return start + (size - total) / 2;
                case Justify::End:    return start + size - total;
                default:              return start;
            }
        };
        const int n = static_cast<int> (kids.size());
        if (layout == ZoneLayout::Circle)
        {
            int maxW = 0, maxH = 0;
            for (const int id : kids) { maxW = std::max (maxW, f.find (id)->w); maxH = std::max (maxH, f.find (id)->h); }
            const double r = std::max (0.0, std::min (cw - maxW, ch - maxH) / 2.0);
            spreadOnCircle (f, kids, midX, midY, r, theta0);
        }
        else if (layout == ZoneLayout::Row || layout == ZoneLayout::Column)
        {
            const bool row = layout == ZoneLayout::Row;
            int sum = 0;
            for (const int id : kids) sum += row ? f.find (id)->w : f.find (id)->h;
            const int mainSize = row ? cw : ch;
            int gap = z.gap;
            if (z.justify == static_cast<int> (Justify::Spread) && n > 1) gap = std::max (0, (mainSize - sum) / (n - 1));
            const int total = sum + gap * (n - 1);
            int pos = z.justify == static_cast<int> (Justify::Spread) ? (row ? cx : cy) : mainStart (row ? cx : cy, mainSize, total);
            for (const int id : kids)
            {
                const auto& w = *f.find (id);
                if (row) { place (id, pos, cross (cy, ch, w.h)); pos += w.w + gap; }
                else     { place (id, cross (cx, cw, w.w), pos); pos += w.h + gap; }
            }
        }
        else
        {
            const int cols = std::min (std::max (1, z.columns), n), rows = (n + cols - 1) / cols;
            int cellW = 0, cellH = 0;
            for (const int id : kids) { cellW = std::max (cellW, f.find (id)->w); cellH = std::max (cellH, f.find (id)->h); }
            const int totalW = cols * cellW + (cols - 1) * z.gap, totalH = rows * cellH + (rows - 1) * z.gap;
            const int x0 = mainStart (cx, cw, totalW), y0 = mainStart (cy, ch, totalH);
            for (int i = 0; i < n; ++i)
            {
                const auto& w = *f.find (kids[static_cast<std::size_t> (i)]);
                const int c = i % cols, r = i / cols;
                place (w.id, cross (x0 + c * (cellW + z.gap), cellW, w.w), cross (y0 + r * (cellH + z.gap), cellH, w.h));
            }
        }
    }
}

void arrange (Facade& f, const std::set<int>& touched)
{
    assignParents (f, touched);
    applyLayouts (f);
}

std::set<int> duplicateWidgets (Facade& f, const std::set<int>& ids, int offset)
{
    std::set<int> all = ids;
    for (const int id : ids) all.merge (descendants (f, id));
    std::map<int, int> remap;
    const auto original = f.widgets;
    for (const auto& src : original)
    {
        if (all.count (src.id) == 0 || static_cast<int> (f.widgets.size()) >= maxWidgets) continue;
        auto copy = src;
        copy.id = f.freeId();
        copy.x += offset;
        copy.y += offset;
        remap[src.id] = copy.id;
        f.widgets.push_back (std::move (copy));
    }
    std::set<int> fresh;
    for (const auto& [from, to] : remap)
    {
        auto* w = f.find (to);
        if (const auto it = remap.find (w->parent); it != remap.end()) w->parent = it->second;   // dans une zone copiée : suit la copie
        if (ids.count (from) != 0) fresh.insert (to);
    }
    f = sanitised (std::move (f));
    return fresh;
}

void matchSize (Facade& f, int id)
{
    const auto* ref = f.find (id);
    if (ref == nullptr) return;
    const int w = ref->w, h = ref->h;
    const auto type = ref->type;
    for (auto& o : f.widgets)
        if (o.type == type) { o.w = w; o.h = h; }
    f = sanitised (std::move (f));
}

namespace
{
json pageJson (const Facade& f)
{
    json o;
    if (! f.image.empty()) o["image"] = f.image;
    o["fit"] = static_cast<int> (f.fit);
    json arr = json::array();
    for (const auto& w : f.widgets)
    {
        json e;
        e["id"] = w.id;
        e["type"] = static_cast<int> (w.type);
        e["x"] = w.x;
        e["y"] = w.y;
        e["w"] = w.w;
        e["h"] = w.h;
        if (w.param != 0) e["param"] = w.param;
        if (! w.text.empty()) e["text"] = w.text;
        if (w.parent != 0) e["parent"] = w.parent;
        if (w.style != 0) e["style"] = w.style;
        if (w.type == WidgetType::Knob || w.type == WidgetType::Slider)
        {
            if (w.stepMode != 0) e["steps"] = w.stepMode;
            if (w.invert != 0) e["invert"] = 1;
            if (w.scale != 0) e["scale"] = w.scale;
            if (! w.response.isLinear()) e["response"] = json::parse (neptune::core::toJson (w.response), nullptr, false);
            if (! w.notches.empty()) e["notches"] = w.notches;
        }
        if (w.type == WidgetType::Slider || w.type == WidgetType::Meter) e["vertical"] = w.vertical;
        if (w.type == WidgetType::Lamp) { e["source"] = w.source; e["ch"] = w.channel; }
        if (w.type == WidgetType::Meter) e["source"] = w.source;
        if (w.type == WidgetType::Action && w.actionMode != 0) e["mode"] = w.actionMode;
        if (w.type == WidgetType::SceneGrid) { e["cols"] = w.columns; e["rows"] = w.rows; e["hue"] = w.hue; e["vivid"] = w.vivid; e["header"] = w.header; }
        if (! w.mirror.empty()) e["mirror"] = w.mirror;
        if (w.type == WidgetType::Midi)
        {
            json m;
            m["kind"] = static_cast<int> (w.midi.kind);
            m["channel"] = w.midi.channel;
            m["d1"] = w.midi.data1;
            m["d2"] = w.midi.data2;
            if (! w.midi.sysex.empty()) m["sysex"] = w.midi.sysex;
            e["midi"] = std::move (m);
            e["target"] = w.midiTarget;
            e["fader"] = w.midiFader;
        }
        if (w.type == WidgetType::Keyboard)
        {
            e["ch"] = w.channel;
            e["first"] = w.firstNote;
            e["hold"] = w.hold;
            e["vel"] = w.velocity;
            if (w.wheels != 0) e["wheels"] = 1;
        }
        if (w.type == WidgetType::Zone)
        {
            e["layout"] = w.layout;
            e["align"] = w.align;
            e["justify"] = w.justify;
            e["gap"] = w.gap;
            e["cols"] = w.columns;
        }
        if (! w.snapshot.empty())
        {
            json snap = json::array();
            for (const auto& [id, v] : w.snapshot) snap.push_back ({ id, v });
            e["snap"] = std::move (snap);
        }
        arr.push_back (std::move (e));
    }
    o["widgets"] = std::move (arr);
    if (! f.styles.empty())
    {
        json st = json::array();
        for (const auto& s : f.styles)
        {
            json e;
            e["id"] = s.id;
            e["name"] = s.name;
            for (const auto& role : imageRoles())
                if (! (s.*(role.field)).empty()) e[role.name] = s.*(role.field);
            if (s.frames != 0) e["frames"] = s.frames;
            if (s.stripHorizontal) e["strip_h"] = true;
            if (s.angleMin != -135 || s.angleMax != 135) e["angles"] = json::array ({ s.angleMin, s.angleMax });
            if (s.pivotX != 50 || s.pivotY != 50) e["pivot"] = json::array ({ s.pivotX, s.pivotY });
            if (s.sliceLeft != 25 || s.sliceTop != 25 || s.sliceRight != 25 || s.sliceBottom != 25) e["slice"] = json::array ({ s.sliceLeft, s.sliceTop, s.sliceRight, s.sliceBottom });
            if (s.sliceScale != 100) e["slice_scale"] = s.sliceScale;
            if (! s.textColour.empty()) e["text_colour"] = s.textColour;
            if (s.textSize != 0) e["text_size"] = s.textSize;
            if (s.textPosition != 0) e["text_position"] = s.textPosition;
            if (! s.textBold) e["text_bold"] = false;
            if (! s.keyLit.empty()) e["key_lit"] = s.keyLit;
            st.push_back (std::move (e));
        }
        o["styles"] = std::move (st);
    }
    return o;
}
} // namespace

std::string toJson (const Facade& f)
{
    json o = pageJson (f);
    if (! f.name.empty()) o["name"] = f.name;
    if (! f.pages.empty())
    {
        json pages = json::array();
        for (const auto& page : f.pages)
        {
            json e = pageJson (page);
            if (! page.name.empty()) e["name"] = page.name;
            pages.push_back (std::move (e));
        }
        o["pages"] = std::move (pages);
    }
    return o.dump();
}

namespace
{
int intOf (const json& o, const char* key, int def)
{
    const auto it = o.find (key);
    return it != o.end() && it->is_number_integer() ? static_cast<int> (std::clamp<long long> (it->get<long long>(), -1000000, 1000000)) : def;
}
} // namespace

namespace
{
Facade pageFromJson (const json& o)
{
    Facade f;
    if (! o.is_object()) return f;
    if (const auto it = o.find ("image"); it != o.end() && it->is_string()) f.image = it->get<std::string>();
    f.fit = static_cast<ImageFit> (intOf (o, "fit", 0));
    if (const auto it = o.find ("styles"); it != o.end() && it->is_array())
        for (const auto& e : *it)
        {
            if (! e.is_object() || f.styles.size() >= static_cast<std::size_t> (maxStyles)) continue;
            ImageStyle s;
            s.id = intOf (e, "id", 0);
            auto str = [&e] (const char* key) { const auto v = e.find (key); return v != e.end() && v->is_string() ? v->get<std::string>() : std::string(); };
            s.name = str ("name");
            for (const auto& role : imageRoles()) s.*(role.field) = str (role.name);
            s.frames = intOf (e, "frames", 0);
            if (const auto h = e.find ("strip_h"); h != e.end() && h->is_boolean()) s.stripHorizontal = h->get<bool>();
            auto pair = [&e] (const char* key, int& a, int& b)
            {
                const auto it = e.find (key);
                if (it != e.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number_integer() && (*it)[1].is_number_integer()) { a = (*it)[0].get<int>(); b = (*it)[1].get<int>(); }
            };
            pair ("angles", s.angleMin, s.angleMax);
            pair ("pivot", s.pivotX, s.pivotY);
            if (const auto it = e.find ("slice"); it != e.end() && it->is_array() && it->size() == 4)
            {
                int* v[4] = { &s.sliceLeft, &s.sliceTop, &s.sliceRight, &s.sliceBottom };
                for (std::size_t i = 0; i < 4; ++i) if ((*it)[i].is_number_integer()) *v[i] = (*it)[i].get<int>();
            }
            s.sliceScale = intOf (e, "slice_scale", 100);
            s.textColour = str ("text_colour");
            s.textSize = intOf (e, "text_size", 0);
            s.textPosition = intOf (e, "text_position", 0);
            if (const auto b = e.find ("text_bold"); b != e.end() && b->is_boolean()) s.textBold = b->get<bool>();
            s.keyLit = str ("key_lit");
            f.styles.push_back (std::move (s));
        }
    if (const auto it = o.find ("widgets"); it != o.end() && it->is_array())
        for (const auto& e : *it)
        {
            if (! e.is_object() || f.widgets.size() >= static_cast<std::size_t> (maxWidgets)) continue;
            Widget w;
            w.id = intOf (e, "id", 0);
            w.type = static_cast<WidgetType> (intOf (e, "type", 0));
            w.x = intOf (e, "x", 0);
            w.y = intOf (e, "y", 0);
            w.w = intOf (e, "w", w.w);
            w.h = intOf (e, "h", w.h);
            w.parent = intOf (e, "parent", 0);
            w.layout = intOf (e, "layout", 0);
            w.align = intOf (e, "align", 0);
            w.justify = intOf (e, "justify", 0);
            w.gap = intOf (e, "gap", 8);
            w.columns = intOf (e, "cols", w.type == WidgetType::SceneGrid ? 4 : 2);
            w.rows = intOf (e, "rows", 4);
            w.hue = intOf (e, "hue", 30);
            w.vivid = intOf (e, "vivid", 55);
            w.header = intOf (e, "header", 1);
            w.style = intOf (e, "style", 0);
            w.stepMode = intOf (e, "steps", 0);
            w.invert = intOf (e, "invert", 0);
            w.scale = intOf (e, "scale", 0);
            if (const auto r = e.find ("response"); r != e.end() && r->is_object()) w.response = velocityCurveFromJson (r->dump());
            if (const auto n = e.find ("notches"); n != e.end() && n->is_array())
                for (const auto& v : *n)
                    if (v.is_number() && w.notches.size() < maxNotches) w.notches.push_back (v.get<double>());
            w.vertical = intOf (e, "vertical", 0);
            w.source = intOf (e, "source", 0);
            w.actionMode = intOf (e, "mode", 0);
            if (const auto m = e.find ("mirror"); m != e.end() && m->is_array())
                for (const auto& v : *m)
                    if (v.is_number_unsigned() && w.mirror.size() < 16) w.mirror.push_back (v.get<ParamId>());
            w.midiTarget = intOf (e, "target", 1);
            w.midiFader = intOf (e, "fader", 0);
            if (const auto m = e.find ("midi"); m != e.end() && m->is_object())
            {
                w.midi.kind = static_cast<midisend::Kind> (intOf (*m, "kind", 2));
                w.midi.channel = intOf (*m, "channel", 1);
                w.midi.data1 = intOf (*m, "d1", 0);
                w.midi.data2 = intOf (*m, "d2", 0);
                if (const auto sx = m->find ("sysex"); sx != m->end() && sx->is_string()) w.midi.sysex = sx->get<std::string>();
            }
            w.channel = intOf (e, "ch", 1);
            w.firstNote = intOf (e, "first", 36);
            w.hold = intOf (e, "hold", 0);
            w.velocity = intOf (e, "vel", 100);
            w.wheels = intOf (e, "wheels", 0);
            if (const auto p = e.find ("param"); p != e.end() && p->is_number_integer())
            {
                const auto v = p->get<long long>();
                if (v > 0 && v < 1000000) w.param = static_cast<ParamId> (v);
            }
            if (const auto t = e.find ("text"); t != e.end() && t->is_string()) w.text = t->get<std::string>();
            if (const auto sn = e.find ("snap"); sn != e.end() && sn->is_array())
                for (const auto& pr : *sn)
                    if (pr.is_array() && pr.size() == 2 && pr[0].is_number_integer() && pr[1].is_number() && w.snapshot.size() < maxSnapshot)
                    {
                        const auto id = pr[0].get<long long>();
                        if (id > 0 && id < 1000000) w.snapshot.emplace_back (static_cast<ParamId> (id), pr[1].get<double>());
                    }
            f.widgets.push_back (std::move (w));
        }
    return f;
}
} // namespace

Facade fromJson (std::string_view text)
{
    const auto o = json::parse (text.begin(), text.end(), nullptr, false);
    if (! o.is_object()) return Facade();
    Facade f = pageFromJson (o);
    if (const auto it = o.find ("name"); it != o.end() && it->is_string()) f.name = it->get<std::string>();
    if (const auto it = o.find ("pages"); it != o.end() && it->is_array())
        for (const auto& e : *it)
        {
            if (! e.is_object() || f.pages.size() + 1 >= static_cast<std::size_t> (maxPages)) continue;
            Facade page = pageFromJson (e);
            if (const auto n = e.find ("name"); n != e.end() && n->is_string()) page.name = n->get<std::string>();
            f.pages.push_back (std::move (page));
        }
    return sanitised (std::move (f));
}

// ---- Pages ----
Facade sanitised (Facade f)
{
    auto pages = std::move (f.pages);
    f.pages.clear();
    f.name = clipped (std::move (f.name), maxTextLength);
    f = sanitisedPage (std::move (f));
    for (auto& page : pages)
    {
        if (f.pages.size() + 1 >= static_cast<std::size_t> (maxPages)) break;
        page.pages.clear();
        page.name = clipped (std::move (page.name), maxTextLength);
        f.pages.push_back (sanitisedPage (std::move (page)));
    }
    return f;
}

std::vector<Facade> splitPages (const Facade& book)
{
    std::vector<Facade> out;
    Facade first = book;
    first.pages.clear();
    out.push_back (std::move (first));
    for (const auto& page : book.pages)
    {
        Facade p = page;
        p.pages.clear();
        out.push_back (std::move (p));
    }
    return out;
}

Facade joinPages (std::vector<Facade> pages)
{
    if (pages.empty()) return Facade();
    if (pages.size() > static_cast<std::size_t> (maxPages)) pages.resize (maxPages);
    Facade book = std::move (pages.front());
    book.pages.assign (std::make_move_iterator (pages.begin() + 1), std::make_move_iterator (pages.end()));
    return book;
}

int pageCount (const Facade& book) noexcept { return 1 + static_cast<int> (book.pages.size()); }

Facade pageOf (const Facade& book, int index)
{
    auto pages = splitPages (book);
    return pages[static_cast<std::size_t> (std::clamp (index, 0, static_cast<int> (pages.size()) - 1))];
}

Facade withPage (const Facade& book, int index, Facade page)
{
    auto pages = splitPages (book);
    page.pages.clear();
    pages[static_cast<std::size_t> (std::clamp (index, 0, static_cast<int> (pages.size()) - 1))] = std::move (page);
    return joinPages (std::move (pages));
}

Facade addPage (const Facade& book, std::string name)
{
    auto pages = splitPages (book);
    if (pages.size() >= static_cast<std::size_t> (maxPages)) return book;
    Facade page;
    page.name = std::move (name);
    pages.push_back (std::move (page));
    return joinPages (std::move (pages));
}

Facade removePage (const Facade& book, int index)
{
    auto pages = splitPages (book);
    if (pages.size() <= 1 || index < 0 || index >= static_cast<int> (pages.size())) return book;
    pages.erase (pages.begin() + index);
    return joinPages (std::move (pages));
}

Facade renamePage (const Facade& book, int index, std::string name)
{
    auto pages = splitPages (book);
    if (index < 0 || index >= static_cast<int> (pages.size())) return book;
    pages[static_cast<std::size_t> (index)].name = std::move (name);
    return joinPages (std::move (pages));
}

Facade movePage (const Facade& book, int index, int target)
{
    auto pages = splitPages (book);
    const int n = static_cast<int> (pages.size());
    if (index < 0 || index >= n || target < 0 || target >= n || index == target) return book;
    auto page = std::move (pages[static_cast<std::size_t> (index)]);
    pages.erase (pages.begin() + index);
    pages.insert (pages.begin() + target, std::move (page));
    return joinPages (std::move (pages));
}
} // namespace neptune::core::facade
