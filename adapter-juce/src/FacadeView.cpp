#include "FacadeView.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <map>

#include "CombiText.h"
#include "neptune/core/MappingEdit.h"
#include "FacadeBundle.h"
#include "ParamFilterBar.h"
#include "VelocityPanel.h"
#include "FacadeExamples.h"
#include "FacadeSpec.h"
#include "StyleLibrary.h"
#include "VelocityPanel.h"

using neptune::core::theme::Token;
namespace fc = neptune::core::facade;

namespace
{
juce::Colour toColour (const neptune::core::theme::Palette& p, Token t)
{
    const auto& c = p.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

constexpr float keyWhiteWidth = 22.0f;   // largeur d'une touche blanche dans le canevas : élargir le clavier ajoute des touches

bool keyIsBlack (int key) noexcept
{
    const int n = key % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// Nombre de touches blanches sous chaque note : la position d'une touche est son rang x largeur.
const std::array<int, 129>& whitesBelow()
{
    static const std::array<int, 129> table = []
    {
        std::array<int, 129> t {};
        int n = 0;
        for (int k = 0; k < 128; ++k)
        {
            t[static_cast<std::size_t> (k)] = n;
            if (! keyIsBlack (k)) ++n;
        }
        t[128] = n;
        return t;
    }();
    return table;
}

// Bord gauche des touches d'un clavier : à droite de la bande des molettes quand il en a.
float keysLeft (const neptune::core::facade::Widget& w) noexcept
{
    return static_cast<float> (w.x) + (w.wheels != 0 ? static_cast<float> (neptune::core::facade::keyboardWheelsWidth) : 0.0f);
}

juce::Rectangle<float> keyRect (const neptune::core::facade::Widget& w, int key)
{
    int first = w.firstNote;
    while (first > 0 && keyIsBlack (first)) --first;
    const auto& below = whitesBelow();
    const float origin = keysLeft (w) - keyWhiteWidth * static_cast<float> (below[static_cast<std::size_t> (first)]);
    const float x = origin + keyWhiteWidth * static_cast<float> (below[static_cast<std::size_t> (key)]);
    const float h = static_cast<float> (w.h);
    if (! keyIsBlack (key)) return { x, static_cast<float> (w.y), keyWhiteWidth, h };
    return { x - 0.3f * keyWhiteWidth, static_cast<float> (w.y), 0.6f * keyWhiteWidth, 0.6f * h };
}

// Touche sous le point (coordonnées du canevas), -1 si aucune ; les noires sont au-dessus des blanches.
int keyAtPoint (const neptune::core::facade::Widget& w, juce::Point<float> p)
{
    if (! juce::Rectangle<float> (keysLeft (w), static_cast<float> (w.y), static_cast<float> (w.x + w.w) - keysLeft (w), static_cast<float> (w.h)).contains (p)) return -1;
    for (const bool black : { true, false })
        for (int k = 0; k < 128; ++k)
            if (keyIsBlack (k) == black && keyRect (w, k).contains (p)) return k;
    return -1;
}


// ---- Grille de scènes : géométrie (partagée par le dessin et les clics), dans les coordonnées du canevas
struct SceneGridGeometry
{
    juce::Rectangle<float> prev, next, label, setPill;   // barre de banque (vides sans barre)
    std::vector<juce::Rectangle<float>> cells;            // une par scène, de gauche à droite et de haut en bas
};

SceneGridGeometry sceneGridGeometry (const neptune::core::facade::Widget& w)
{
    SceneGridGeometry g;
    const juce::Rectangle<float> b (static_cast<float> (w.x), static_cast<float> (w.y), static_cast<float> (w.w), static_cast<float> (w.h));
    auto area = b.reduced (4.0f);
    if (w.header != 0)
    {
        auto bar = area.removeFromTop (34.0f);
        area.removeFromTop (6.0f);
        g.prev = bar.removeFromLeft (52.0f);
        if (w.setPill != 0) g.setPill = bar.removeFromRight (76.0f);
        g.next = bar.removeFromRight (52.0f);
        g.label = bar;
    }
    const int cols = std::clamp (w.columns, 4, 8), rows = std::clamp (w.rows, 4, 8);
    const float gap = 6.0f;
    const float cw = (area.getWidth() - gap * static_cast<float> (cols - 1)) / static_cast<float> (cols);
    const float ch = (area.getHeight() - gap * static_cast<float> (rows - 1)) / static_cast<float> (rows);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            g.cells.push_back ({ area.getX() + static_cast<float> (c) * (cw + gap), area.getY() + static_cast<float> (r) * (ch + gap), cw, ch });
    return g;
}

// Couleurs d'une case : une teinte, et un « tempérament » (douce à vive) qui règle la saturation et la luminosité ; la scène rappelée est la plus claire.
juce::Colour sceneColour (const neptune::core::facade::Widget& w, bool lit)
{
    const float v = static_cast<float> (std::clamp (w.vivid, 0, 100)) / 100.0f;
    const float hue = static_cast<float> (w.hue % 360) / 360.0f;
    return lit ? juce::Colour::fromHSV (hue, 0.35f + 0.60f * v, 0.65f + 0.35f * v, 1.0f)
               : juce::Colour::fromHSV (hue, 0.25f + 0.55f * v, 0.26f + 0.24f * v, 1.0f);
}

juce::Colour readableOn (juce::Colour fill) { return fill.getPerceivedBrightness() > 0.55f ? juce::Colours::black.withAlpha (0.88f) : juce::Colours::white.withAlpha (0.95f); }


// Texte dans un cadre : la taille voulue, réduite au besoin pour que tout tienne (nombre de lignes et hauteur) ; le haut du texte colle au haut du cadre quand `top` est vrai.
void drawAutoFit (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float wantedSize, bool bold, int maxLines, bool top, juce::Colour colour)
{
    if (text.isEmpty() || area.getWidth() < 4.0f || area.getHeight() < 4.0f) return;
    for (float size = std::min (wantedSize, area.getHeight()); size >= 7.0f; size *= 0.93f)
    {
        juce::AttributedString as;
        as.setJustification (juce::Justification::centredTop);
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (text, juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)), colour);
        juce::TextLayout layout;
        layout.createLayout (as, area.getWidth());
        if (layout.getNumLines() > maxLines || layout.getHeight() > area.getHeight() || layout.getWidth() > area.getWidth() + 1.0f)
            if (size > 7.5f) continue;
        const float y = top ? area.getY() : area.getY() + (area.getHeight() - layout.getHeight()) * 0.5f;
        layout.draw (g, juce::Rectangle<float> (area.getX(), y, area.getWidth(), layout.getHeight() + 2.0f));
        return;
    }
}

constexpr float curveTitleHeight = 24.0f;   // hauteur (dans le canevas) de la légende d'un widget Courbe

int snapped (int v) { return (v + 2) / 4 * 4; } // aimantation sur une grille de 4

juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

bool needsParam (fc::WidgetType t) { return t == fc::WidgetType::Knob || t == fc::WidgetType::Button || t == fc::WidgetType::Menu || t == fc::WidgetType::Slider || t == fc::WidgetType::Lamp; }

// Les widgets qui peuvent prendre un style d'images.
bool takesStyle (fc::WidgetType t)
{
    return t == fc::WidgetType::Knob || t == fc::WidgetType::Button || t == fc::WidgetType::Action || t == fc::WidgetType::Slider
        || t == fc::WidgetType::Keyboard || t == fc::WidgetType::Zone || t == fc::WidgetType::Lamp || t == fc::WidgetType::Set || t == fc::WidgetType::Midi;
}

// Charge une image PNG, JPG ou SVG (le SVG est rastérisé, côté le plus long = maxSide), depuis un fichier ou un pack de styles
// (référence « pack:... »). Image vide si introuvable ou illisible. `natural` reçoit la taille naturelle du dessin (SVG : sa taille
// déclarée ; PNG et JPG : ses pixels). Dans un SVG, les repères {{accent}}, {{text}}... sont remplacés par les couleurs du thème (`vars`).
juce::Image loadPicture (const std::string& path, int maxSide, const std::map<std::string, std::string>& vars, juce::Point<float>& natural)
{
    juce::MemoryBlock data;
    natural = {};
    if (! stylelib::readBytes (path, data) || data.getSize() == 0) return {};
    if (juce::String::fromUTF8 (path.c_str()).endsWithIgnoreCase (".svg"))
    {
        auto svg = juce::String::createStringFromData (data.getData(), static_cast<int> (data.getSize()));
        if (svg.contains ("{{"))
            for (const auto& [name, colour] : vars) svg = svg.replace ("{{" + juce::String (name) + "}}", juce::String (colour));
        const std::unique_ptr<juce::Drawable> drawable (juce::Drawable::createFromImageData (svg.toRawUTF8(), svg.getNumBytesAsUTF8()));
        if (drawable == nullptr) return {};
        const auto bounds = drawable->getDrawableBounds();
        if (bounds.isEmpty()) return {};
        natural = { bounds.getWidth(), bounds.getHeight() };
        const float k = static_cast<float> (maxSide) / std::max (bounds.getWidth(), bounds.getHeight());
        const int w = std::max (1, juce::roundToInt (bounds.getWidth() * k)), h = std::max (1, juce::roundToInt (bounds.getHeight() * k));
        juce::Image img (juce::Image::ARGB, w, h, true);
        juce::Graphics g (img);
        drawable->drawWithin (g, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (w), static_cast<float> (h)), juce::RectanglePlacement::centred, 1.0f);
        return img;
    }
    auto img = juce::ImageFileFormat::loadFrom (data.getData(), data.getSize());
    if (img.isValid()) natural = { static_cast<float> (img.getWidth()), static_cast<float> (img.getHeight()) };
    return img;
}

// « #rrggbb » ou « #aarrggbb » ; `fallback` si illisible.
juce::Colour parseColour (const std::string& text, juce::Colour fallback)
{
    auto t = juce::String (text).trim().trimCharactersAtStart ("#");
    if (t.length() == 6 && t.containsOnly ("0123456789abcdefABCDEF")) return juce::Colour (0xff000000u | static_cast<juce::uint32> (t.getHexValue32()));
    if (t.length() == 8 && t.containsOnly ("0123456789abcdefABCDEF")) return juce::Colour (static_cast<juce::uint32> (t.getHexValue32()));
    return fallback;
}

// Texte posé sur un widget, avec les réglages du style (couleur, taille, place, gras). `ratio` : taille par défaut, en part de la hauteur.
void drawStyledText (juce::Graphics& g, const fc::ImageStyle* style, const juce::String& text, juce::Rectangle<float> area, float ratio, juce::Colour defaultColour,
                     juce::Justification defaultPlace = juce::Justification::centred)
{
    if (text.isEmpty()) return;
    if (style != nullptr && style->textSize > 0) ratio = static_cast<float> (style->textSize) / 100.0f;
    const float size = juce::jlimit (9.0f, 80.0f, area.getHeight() * ratio);
    g.setColour (style != nullptr ? parseColour (style->textColour, defaultColour) : defaultColour);
    g.setFont (juce::FontOptions (size, style == nullptr || style->textBold ? juce::Font::bold : juce::Font::plain));
    juce::Justification place = defaultPlace;
    if (style != nullptr)
        switch (style->textPosition)
        {
            case 1: place = juce::Justification::centredTop; break;
            case 2: place = juce::Justification::centredBottom; break;
            case 3: place = juce::Justification::centredLeft; break;
            case 4: place = juce::Justification::centredRight; break;
            default: break;
        }
    g.drawFittedText (text, area.toNearestInt().reduced (4), place, 2);
}

// Image découpée en neuf zones : les coins gardent leur taille, les bords et le centre s'étirent. Les marges de découpe sont des
// pourcentages de l'image ; les coins s'affichent à leur taille naturelle (en unités du canevas) multipliée par `sliceScale`.
void drawNineSlice (juce::Graphics& g, const juce::Image& img, juce::Point<float> natural, const fc::ImageStyle& st, juce::Rectangle<float> dest)
{
    if (! img.isValid() || natural.x <= 0.0f || natural.y <= 0.0f) return;
    const float iw = static_cast<float> (img.getWidth()), ih = static_cast<float> (img.getHeight());
    const float k = static_cast<float> (st.sliceScale) / 100.0f;
    float dl = natural.x * static_cast<float> (st.sliceLeft) / 100.0f * k, dr = natural.x * static_cast<float> (st.sliceRight) / 100.0f * k;
    float dt = natural.y * static_cast<float> (st.sliceTop) / 100.0f * k, db = natural.y * static_cast<float> (st.sliceBottom) / 100.0f * k;
    if (dl + dr > dest.getWidth()) { const float f = dest.getWidth() / (dl + dr); dl *= f; dr *= f; }
    if (dt + db > dest.getHeight()) { const float f = dest.getHeight() / (dt + db); dt *= f; db *= f; }
    const int sx[4] = { 0, juce::roundToInt (iw * static_cast<float> (st.sliceLeft) / 100.0f), juce::roundToInt (iw * (1.0f - static_cast<float> (st.sliceRight) / 100.0f)), img.getWidth() };
    const int sy[4] = { 0, juce::roundToInt (ih * static_cast<float> (st.sliceTop) / 100.0f), juce::roundToInt (ih * (1.0f - static_cast<float> (st.sliceBottom) / 100.0f)), img.getHeight() };
    const int dx[4] = { juce::roundToInt (dest.getX()), juce::roundToInt (dest.getX() + dl), juce::roundToInt (dest.getRight() - dr), juce::roundToInt (dest.getRight()) };
    const int dy[4] = { juce::roundToInt (dest.getY()), juce::roundToInt (dest.getY() + dt), juce::roundToInt (dest.getBottom() - db), juce::roundToInt (dest.getBottom()) };
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
        {
            const int sw = sx[col + 1] - sx[col], sh = sy[row + 1] - sy[row], dw = dx[col + 1] - dx[col], dh = dy[row + 1] - dy[row];
            if (sw > 0 && sh > 0 && dw > 0 && dh > 0) g.drawImage (img, dx[col], dy[row], dw, dh, sx[col], sy[row], sw, sh);
        }
}

// Taille de la poignée d'un curseur le long de son axe : celle de son image, sinon un rectangle étroit.
float sliderHandleLength (const fc::Widget& w, const juce::Image& handle)
{
    const float cross = static_cast<float> (w.vertical != 0 ? w.w : w.h);
    const float aspect = handle.isValid() ? static_cast<float> (handle.getWidth()) / static_cast<float> (handle.getHeight()) : (w.vertical != 0 ? 2.0f : 0.5f);   // largeur / hauteur
    const float len = w.vertical != 0 ? cross / aspect : cross * aspect;
    return std::clamp (len, 10.0f, static_cast<float> (w.vertical != 0 ? w.h : w.w) * 0.5f);
}

// Valeurs proposées par un menu : un paramètre à choix ou à pas (rendu vide pour les autres).
int menuCount (const neptune::core::ParamSpec& s)
{
    if (s.kind == neptune::core::Kind::Choice) return static_cast<int> (s.choices.size());
    if (s.kind == neptune::core::Kind::Stepped) return std::clamp (static_cast<int> (s.max - s.min) + 1, 0, 256);
    return 0;
}
} // namespace

// ============================== Toile : dessin des widgets et gestes ==============================

class FacadeView::Canvas : public juce::Component, private juce::Timer
{
public:
    void setTopAligned (bool on) { if (topAligned_ != on) { topAligned_ = on; repaint(); } }

    explicit Canvas (FacadeView& owner) : owner_ (owner)
    {
        setWantsKeyboardFocus (true);
        auto hex = [&owner] (Token t) { return toColour (owner.palette_, t).toDisplayString (false).paddedLeft ('0', 6).toStdString(); };
        static const std::pair<const char*, Token> table[] = { { "accent", Token::Accent }, { "accent2", Token::Accent2 }, { "text", Token::Text }, { "text_dim", Token::TextDim },
                                                               { "bg", Token::BgApp }, { "card", Token::BgCard }, { "panel", Token::PanelBg }, { "border", Token::Border },
                                                               { "hot", Token::KnobHot }, { "face", Token::KnobFace } };
        for (const auto& [name, token] : table)
            svgVars_[name] = "#" + hex (token);
        bankHold_.fire = [this]   // appui long sur une flèche de banque : au bout d'une demi-seconde la banque change, puis elle continue de défiler tant que l'appui dure
        {
            if (bankDir_ == 0) { bankHold_.stopTimer(); return; }
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - bankHoldStartMs_ >= 500.0)
            {
                namespace cb = neptune::core::combi;
                owner_.processor_.uiCombiCommand (bankDir_ < 0 ? cb::kCommandBankPrevious : cb::kCommandBankNext, combiTrack());
                bankHoldStartMs_ = now;
            }
            repaint();
        };
    }
    ~Canvas() override { bankHold_.stopTimer(); releaseKeys(); }

    bool dragging() const noexcept { return mode_ != Mode::None; }

    // Image du cache, pour l'aperçu de la fenêtre des styles.
    juce::Image cachedPicture (const std::string& path, int maxSide = 1024) const { return picture (path, maxSide); }

    // Vumètres : lit les niveaux de la piste ou du Master et les lisse (descente de 1,5 dB par image, maintien de crête puis chute).
    void tickMeters()
    {
        auto& o = owner_;
        auto toDb = [] (float v) { return v > 1.0e-5f ? 20.0f * std::log10 (v) : -100.0f; };
        for (const auto& w : o.facade_.widgets)
        {
            if (w.type != fc::WidgetType::Meter) continue;
            const auto src = fc::decodeMeterSource (w.source);
            const int index = src.index < 0 ? o.track_ : src.index;
            float now[2] = { -100.0f, -100.0f };
            if (src.reduction)
            {
                // réduction de gain (dB) -> position sur l'échelle -60..0 de la barre : 2 dB de barre par dB de réduction (0..30 dB)
                using Kind = fc::MeterSource::Kind;
                const auto kind = src.kind == Kind::Master ? NeptuneProcessor::StripKind::Master : src.kind == Kind::Bus ? NeptuneProcessor::StripKind::Bus
                                : src.kind == Kind::Parallel ? NeptuneProcessor::StripKind::Parallel : NeptuneProcessor::StripKind::Track;
                if (index >= 0 || src.kind == Kind::Master)
                {
                    const auto gr = o.processor_.compressorReduction (kind, std::max (0, index));
                    now[0] = -60.0f + 2.0f * gr.first;
                    now[1] = -60.0f + 2.0f * gr.second;
                }
            }
            else
            {
                neptune::core::Engine::Meter m;
                if (src.kind == fc::MeterSource::Kind::Master) m = o.processor_.masterMeter();
                else if (src.kind == fc::MeterSource::Kind::Bus) m = o.processor_.busMeter (index);
                else if (src.kind == fc::MeterSource::Kind::Parallel) m = o.processor_.parallelMeter (index);
                else if (index >= 0) m = o.processor_.trackMeter (index);
                now[0] = toDb (m.peakL);
                now[1] = toDb (m.peakR);
            }
            auto& s = meters_[w.id];
            for (int i = 0; i < 2; ++i)
            {
                s.level[i] = std::max (now[i], s.level[i] - 1.5f);
                if (now[i] >= s.hold[i]) { s.hold[i] = now[i]; s.age[i] = 0; }
                else if (++s.age[i] > 20) s.hold[i] = std::max (now[i], s.hold[i] - 0.8f);
            }
        }
    }

    // Oublie les images chargées (un style a changé ou un fichier a été remplacé).
    void clearPictures() { pictures_.clear(); repaint(); }

    // Relâche toutes les notes tenues par les claviers (changement de piste, sortie du mode Jeu, fermeture).
    void releaseKeys()
    {
        auto& o = owner_;
        if (true)   // aussi dans la Façade globale (piste -1) : ses claviers jouent par le moteur
        {
            for (const auto& kv : pressed_)
                if (const auto* w = o.facade_.find (kv.second.first)) noteOff (*w, kv.second.second);
            for (const auto& [id, keys] : latched_)
                if (const auto* w = o.facade_.find (id))
                    for (const int k : keys) noteOff (*w, k);
        }
        pressed_.clear();
        latched_.clear();
        if (mode_ == Mode::Key) mode_ = Mode::None;
        repaint();
    }

    void resized() override { syncChildren(); }

    // Courbes de vélocité : un panneau réel par widget « Courbe » (mode Jeu), posé sur la toile. C'est la courbe d'entrée de la piste
    // (réglages de bande), valable pour tous les types de piste : elle remappe la vélocité MIDI avant n'importe quel moteur.
    void syncChildren()
    {
        auto& o = owner_;
        std::set<int> wanted;
        if (o.track_ >= 0 && ! o.editMode_)
            for (const auto& w : o.facade_.widgets)
                if (w.type == fc::WidgetType::Curve) wanted.insert (w.id);
        for (auto it = panels_.begin(); it != panels_.end();)
            it = wanted.count (it->first) != 0 ? std::next (it) : panels_.erase (it);

        const auto rev = o.processor_.instrumentRevision();
        const bool refreshValues = rev != panelRevision_;
        panelRevision_ = rev;
        for (const int id : wanted)
        {
            const auto* w = o.facade_.find (id);
            if (w == nullptr) continue;
            auto& panel = panels_[id];
            if (panel == nullptr)
            {
                panel = std::make_unique<VelocityPanel> (o.palette_, o.tr_, 0, false, o.processor_.trackSettings (o.track_).velocityCurve);
                auto* proc = &o.processor_;
                panel->setEnabledToggleVisible (true);   // la courbe d'entrée peut être mise de côté sans perdre ses réglages
                panel->setFadeControlsVisible (false);   // la Façade n'expose que la courbe de transfert ; le fondu reste dans l'éditeur Sampler
                panel->onGestureBegin = [proc] { proc->uiBeginGesture(); };
                panel->onGestureEnd = [proc] { proc->uiEndGesture(); };
                panel->onUndo = [proc] { proc->uiUndo(); };
                panel->onRedo = [proc] { proc->uiRedo(); };
                panel->onChange = [proc] (int, bool, const neptune::core::VelocityCurve& curve)
                {
                    const int track = proc->selectedTrack();
                    auto strip = proc->trackSettings (track);
                    strip.velocityCurve = curve;
                    proc->uiEditTrack (track, std::move (strip), "Velocity curve", "velocity_curve");
                };
                addAndMakeVisible (*panel);
            }
            else if (refreshValues)
            {
                panel->setValues (0, false, o.processor_.trackSettings (o.track_).velocityCurve);
            }
            const auto area = canvasArea();
            const float s = scale();
            const float title = w->text.empty() ? 0.0f : curveTitleHeight;   // la légende de la courbe est au-dessus du panneau
            panel->setBounds (juce::Rectangle<float> (area.getX() + w->x * s, area.getY() + (w->y + title) * s, w->w * s, (w->h - title) * s).toNearestInt());
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto& o = owner_;
        const auto area = canvasArea();
        g.setColour (o.colour (Token::PanelBg));
        g.fillRoundedRectangle (area, 10.0f);

        const juce::Image image_ = picture (o.facade_.image, 2048);
        {
            juce::Graphics::ScopedSaveState keep (g);
            juce::Path rounded;
            rounded.addRoundedRectangle (area, 10.0f);
            g.reduceClipRegion (rounded);
            if (image_.isValid())
            {
                const float iw = static_cast<float> (image_.getWidth()), ih = static_cast<float> (image_.getHeight());
                auto dest = area;
                if (o.facade_.fit == fc::ImageFit::Fit || o.facade_.fit == fc::ImageFit::Fill)
                {
                    const float k = o.facade_.fit == fc::ImageFit::Fit ? std::min (area.getWidth() / iw, area.getHeight() / ih)
                                                                       : std::max (area.getWidth() / iw, area.getHeight() / ih);
                    dest = juce::Rectangle<float> (iw * k, ih * k).withCentre (area.getCentre());
                }
                g.drawImage (image_, dest, juce::RectanglePlacement::stretchToFit);
            }

            g.addTransform (juce::AffineTransform::scale (scale()).translated (area.getX(), area.getY()));
            for (const auto& w : o.facade_.widgets)
                if (w.type == fc::WidgetType::Zone) paintWidget (g, w);   // les zones sont derrière leur contenu
            for (const auto& w : o.facade_.widgets)
                if (w.type != fc::WidgetType::Zone) paintWidget (g, w);
            if (o.editMode_)
            {
                for (const auto& w : o.facade_.widgets) paintEditOverlay (g, w);
                if (mode_ == Mode::Band)
                {
                    g.setColour (o.colour (Token::Accent).withAlpha (0.18f));
                    g.fillRect (band_);
                    g.setColour (o.colour (Token::Accent));
                    g.drawRect (band_, 1.0f);
                }
            }
        }
        g.setColour (o.colour (Token::Pb));
        g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);

        if (o.facade_.widgets.empty())
        {
            g.setColour (o.colour (Token::TextDim));
            g.setFont (juce::FontOptions (16.0f));
            g.drawFittedText (o.text ("facade.empty"), area.toNearestInt().reduced (40), juce::Justification::centred, 4);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto& o = owner_;
        const auto p = toCanvas (e.position);
        if (o.editMode_) { editDown (p, e.mods); return; }

        const auto* w = widgetAt (p);
        if (w == nullptr) return;
        if (w->type == fc::WidgetType::Keyboard) { if (! e.mods.isPopupMenu()) keyDown (e, *w); return; }
        if (w->type == fc::WidgetType::SceneGrid) { sceneGridDown (e, *w); return; }
        if (w->type == fc::WidgetType::Midi)
        {
            if (e.mods.isPopupMenu()) return;
            midiId_ = w->id;
            midiLast_.erase (w->id);   // chaque geste commence par envoyer sa valeur
            mode_ = Mode::Midi;
            if (w->midiFader != 0) sendMidiFader (*w, toCanvas (e.position)); else { sendMidi (*w, false); repaint(); }
            return;
        }
        if (w->type == fc::WidgetType::Set)
        {
            if (! e.mods.isPopupMenu()) { o.processor_.uiCombiArm (! o.processor_.combiArmed()); repaint(); }
            return;
        }
        if (w->type == fc::WidgetType::Action && w->actionMode >= 2)
        {
            // commande de combinaison d'orgue : agit à l'appui (piston, Set, annulation, séquenceur, banque)
            if (! e.mods.isPopupMenu()) { o.processor_.uiCombiCommand (w->actionMode - 1, combiTrack()); repaint(); }
            return;
        }
        if (w->type == fc::WidgetType::Action)
        {
            if (e.mods.isPopupMenu()) return;
            if (o.processor_.combiArmed() && w->actionMode == 0) { storePiston (w->id); return; }   // « Set » armé : ce piston mémorise les valeurs courantes
            // le rappel a lieu au relâchement ; un appui long (0,7 s) mémorise à la place, comme « Set » + piston
            pistonId_ = w->id;
            longPress_ = false;
            startTimer (700);
            return;
        }
        if (w->param == 0 || o.processor_.parameters().indexOf (w->param) == neptune::core::ParameterSet::npos) return;
        if (e.mods.isPopupMenu())
        {
            if (w->type == fc::WidgetType::Knob) o.openModal_ (w->param, neptune::core::uistate::Terminal::Pointer);
            return;
        }
        playId_ = w->param;
        if (w->type == fc::WidgetType::Slider)
        {
            o.processor_.uiBeginEdit (w->param);
            mode_ = Mode::Slide;
            slideId_ = w->id;
            slideTo (*w, p);
        }
        else if (w->type == fc::WidgetType::Button)
        {
            toggle (w->param, w->mirror);
            playId_ = 0;
        }
        else if (w->type == fc::WidgetType::Menu)
        {
            showValueMenu (*w);
            playId_ = 0;
        }
        else if (w->type == fc::WidgetType::Knob)
        {
            o.processor_.uiBeginEdit (w->param);
            mode_ = Mode::Turn;
            startNorm_ = shownPosition (*w);
            turnId_ = w->id;
            startY_ = e.position.y;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& o = owner_;
        if (mode_ == Mode::Key)
        {
            const auto it = pressed_.find (e.source.getIndex());
            if (it == pressed_.end()) return;
            const auto* w = o.facade_.find (it->second.first);
            if (w == nullptr) return;
            const int key = keyAtPoint (*w, toCanvas (e.position));
            if (key >= 0 && key != it->second.second)   // glissando : la note change quand le doigt passe sur une autre touche
            {
                o.processor_.uiKeyOff (o.track_, it->second.second, w->channel - 1);
                it->second.second = key;
                o.processor_.uiKeyOn (o.track_, key, static_cast<float> (w->velocity) / 127.0f, w->channel - 1);
                repaint();
            }
            return;
        }
        if (mode_ == Mode::Wheel)
        {
            if (const auto* w = o.facade_.find (wheelWidget_)) wheelSet (*w, wheelIndex_, toCanvas (e.position).y);
            return;
        }
        if (mode_ == Mode::Midi)
        {
            if (const auto* w = o.facade_.find (midiId_); w != nullptr && w->midiFader != 0) sendMidiFader (*w, toCanvas (e.position));
            return;
        }
        if (mode_ == Mode::Slide)
        {
            if (const auto* w = o.facade_.find (slideId_)) slideTo (*w, toCanvas (e.position));
            return;
        }
        if (mode_ == Mode::Turn)
        {
            const double range = e.mods.isShiftDown() ? 1000.0 : 200.0;
            if (const auto* w = o.facade_.find (turnId_))
                o.processor_.uiSetNormalised (playId_, normalisedFor (*w, juce::jlimit (0.0, 1.0, startNorm_ + (startY_ - e.position.y) / range), false));
            repaint();
            return;
        }
        const auto p = toCanvas (e.position);
        if (mode_ == Mode::Band)
        {
            band_ = juce::Rectangle<float> (dragStart_, p);
            o.selection_ = bandBase_;
            for (const auto& w : before_.widgets)
                if (band_.intersects (juce::Rectangle<float> (static_cast<float> (w.x), static_cast<float> (w.y), static_cast<float> (w.w), static_cast<float> (w.h))))
                    o.selection_.insert (w.id);
            o.updateButtons();
            repaint();
            return;
        }
        if (mode_ != Mode::Move && mode_ != Mode::Resize) return;
        const int dx = juce::roundToInt (p.x - dragStart_.x), dy = juce::roundToInt (p.y - dragStart_.y);
        if (mode_ == Mode::Move)
        {
            // le groupe se déplace d'un bloc et reste dans le canevas
            int minX = fc::canvasWidth, minY = fc::canvasHeight, maxR = 0, maxB = 0;
            for (const auto& w : before_.widgets)
                if (moving_.count (w.id) != 0) { minX = std::min (minX, w.x); minY = std::min (minY, w.y); maxR = std::max (maxR, w.x + w.w); maxB = std::max (maxB, w.y + w.h); }
            const int mx = std::clamp (snapped (dx), -minX, fc::canvasWidth - maxR);
            const int my = std::clamp (snapped (dy), -minY, fc::canvasHeight - maxB);
            for (auto& w : o.facade_.widgets)
                if (const auto* b = before_.find (w.id); b != nullptr && moving_.count (w.id) != 0) { w.x = b->x + mx; w.y = b->y + my; }
        }
        else if (auto* w = o.facade_.find (o.single()))
        {
            w->w = std::clamp (snapped (startRect_.getWidth() + dx), fc::minWidgetSize, fc::canvasWidth - w->x);
            w->h = std::clamp (snapped (startRect_.getHeight() + dy), fc::minWidgetSize, fc::canvasHeight - w->y);
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto& o = owner_;
        if (bankDir_ != 0) { bankHold_.stopTimer(); bankDir_ = 0; bankHoldId_ = 0; repaint(); }
        if (pistonId_ != 0)
        {
            stopTimer();
            const int id = pistonId_;
            pistonId_ = 0;
            if (! longPress_)
                if (const auto* w = o.facade_.find (id)) runAction (*w);
            longPress_ = false;
            return;
        }
        if (mode_ == Mode::Wheel)
        {
            if (const auto* w = o.facade_.find (wheelWidget_); w != nullptr && wheelIndex_ == 0) sendWheel (*w, 0, 0.0f);   // la molette de hauteur revient au milieu
            wheelWidget_ = 0;
            mode_ = Mode::None;
            repaint();
            return;
        }
        if (mode_ == Mode::Key)
        {
            const auto it = pressed_.find (e.source.getIndex());
            if (it != pressed_.end())
            {
                if (const auto* w = o.facade_.find (it->second.first)) o.processor_.uiKeyOff (o.track_, it->second.second, w->channel - 1);
                pressed_.erase (it);
            }
            if (pressed_.empty()) mode_ = Mode::None;
            repaint();
            return;
        }
        if (mode_ == Mode::Midi)
        {
            if (const auto* w = o.facade_.find (midiId_); w != nullptr && w->midiFader == 0 && w->midi.kind == neptune::core::midisend::Kind::NoteOn) sendMidi (*w, true);
            midiId_ = 0;
            mode_ = Mode::None;
            repaint();
            return;
        }
        const auto mode = mode_;
        mode_ = Mode::None;
        if (mode == Mode::Turn || mode == Mode::Slide)
        {
            o.processor_.uiEndEdit (playId_);
            playId_ = 0;
        }
        else if (mode == Mode::Band)
            repaint();
        else if (mode == Mode::Move || mode == Mode::Resize)
        {
            if (o.facade_ != before_)
            {
                auto f = o.facade_;
                o.commitArranged (std::move (f), mode == Mode::Move ? o.selection_ : std::set<int> { o.single() }, mode == Mode::Move ? "Facade: move widgets" : "Facade: resize widget");
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto* w = owner_.editMode_ ? nullptr : widgetAt (toCanvas (e.position));
        const int id = w != nullptr ? w->id : 0;
        if (id != hoverId_) { hoverId_ = id; repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { if (hoverId_ != 0) { hoverId_ = 0; repaint(); } }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto& o = owner_;
        const auto* w = widgetAt (toCanvas (e.position));
        if (w == nullptr) return;
        if (o.editMode_)
        {
            if (w->type == fc::WidgetType::Midi) o.openMidiEditor();
            else if (w->type == fc::WidgetType::Action) o.openPicker (true);
            else if (needsParam (w->type)) o.openPicker();
            return;
        }
        if ((w->type == fc::WidgetType::Knob || w->type == fc::WidgetType::Slider) && w->param != 0) o.processor_.uiResetToDefault (w->param);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        auto& o = owner_;
        if (o.editMode_) return;
        const auto* w = widgetAt (toCanvas (e.position));
        if (w == nullptr || (w->type != fc::WidgetType::Knob && w->type != fc::WidgetType::Slider) || w->param == 0) return;
        auto& ps = o.processor_.parameters();
        if (ps.indexOf (w->param) == neptune::core::ParameterSet::npos) return;
        const auto* spec = ps.find (w->param);
        const bool custom = w->stepMode == 2 && w->notches.size() >= 2;
        const bool stepped = custom || (w->stepMode == 0 && spec != nullptr && spec->max > spec->min && (spec->kind == neptune::core::Kind::Choice || spec->kind == neptune::core::Kind::Stepped));
        const double cran = custom ? 1.0 / static_cast<double> (w->notches.size() - 1) : spec != nullptr && spec->max > spec->min ? 1.0 / (spec->max - spec->min) : 0.1;
        const double delta = stepped ? (wheel.deltaY > 0 ? 1.0 : -1.0) * cran : wheel.deltaY * 0.1;   // un cran par molette quand le widget a des crans
        o.processor_.uiBeginEdit (w->param);
        const double wheelNorm = normalisedFor (*w, juce::jlimit (0.0, 1.0, shownPosition (*w) + delta), w->type == fc::WidgetType::Slider);
        o.processor_.uiSetNormalised (w->param, wheelNorm);
        for (const auto id : w->mirror) o.processor_.uiSetNormalised (id, wheelNorm);
        o.processor_.uiEndEdit (w->param);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        auto& o = owner_;
        if (! o.editMode_) return false;
        if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a'))
        {
            for (const auto& w : o.facade_.widgets) o.selection_.insert (w.id);
            o.updateButtons();
            repaint();
            return true;
        }
        if (o.selection_.empty()) return false;
        const int step = k.getModifiers().isShiftDown() ? 10 : 1;
        int dx = 0, dy = 0;
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { o.deleteSelected(); return true; }
        if (k.getKeyCode() == juce::KeyPress::leftKey) dx = -step;
        else if (k.getKeyCode() == juce::KeyPress::rightKey) dx = step;
        else if (k.getKeyCode() == juce::KeyPress::upKey) dy = -step;
        else if (k.getKeyCode() == juce::KeyPress::downKey) dy = step;
        else return false;
        auto f = o.facade_;
        const auto ids = movingSet();
        int minX = fc::canvasWidth, minY = fc::canvasHeight, maxR = 0, maxB = 0;
        for (const auto& w : f.widgets)
            if (ids.count (w.id) != 0) { minX = std::min (minX, w.x); minY = std::min (minY, w.y); maxR = std::max (maxR, w.x + w.w); maxB = std::max (maxB, w.y + w.h); }
        dx = std::clamp (dx, -minX, fc::canvasWidth - maxR);
        dy = std::clamp (dy, -minY, fc::canvasHeight - maxB);
        fc::moveWidgets (f, o.selection_, dx, dy);
        o.commitArranged (std::move (f), o.selection_, "Facade: move widgets");
        return true;
    }

private:
    enum class Mode { None, Move, Resize, Turn, Band, Key, Slide, Midi, Wheel };

    // Message MIDI d'un widget « Message MIDI » vers Neptune et/ou l'hôte. `release` : le relâchement d'une note envoie la note coupée.
    void sendMidi (const fc::Widget& w, bool release)
    {
        auto m = w.midi;
        if (release) { m.kind = neptune::core::midisend::Kind::NoteOff; m.data2 = 0; }
        const auto bytes = neptune::core::midisend::encode (m);
        if (! bytes.empty()) owner_.processor_.postMidi (bytes, w.midiTarget);
    }

    // Curseur MIDI : la position du geste donne la valeur (0..127, ou 0..16383 pour un pitch bend) ; un message part à chaque changement.
    void sendMidiFader (const fc::Widget& w, juce::Point<float> p)
    {
        using neptune::core::midisend::Kind;
        const double t = std::clamp ((p.x - static_cast<float>(w.x)) / static_cast<float> (std::max (1, w.w)), 0.0f, 1.0f);
        auto m = w.midi;
        int value;
        if (m.kind == Kind::PitchBend) { value = static_cast<int> (std::lround (t * 16383.0)); m.data1 = value; }
        else
        {
            value = static_cast<int> (std::lround (t * 127.0));
            if (m.kind == Kind::ProgramChange || m.kind == Kind::ChannelPressure) m.data1 = value; else m.data2 = value;
        }
        if (const auto it = midiLast_.find (w.id); it != midiLast_.end() && it->second == value) return;
        midiLast_[w.id] = value;
        midiFader_[w.id] = t;
        const auto bytes = neptune::core::midisend::encode (m);
        if (! bytes.empty()) owner_.processor_.postMidi (bytes, w.midiTarget);
        repaint();
    }

    // Position affichée (0..1, dans le sens de l'écran) d'un potard ou d'un curseur : celle du paramètre, ou celle de son cran le plus
    // proche si le widget a des crans personnalisés ; inversée si le widget est inversé.
    double shownPosition (const fc::Widget& w) const
    {
        auto& ps = owner_.processor_.parameters();
        const auto* spec = w.param != 0 ? ps.find (w.param) : nullptr;
        if (spec == nullptr) return 0.0;
        const double t = w.stepMode == 2 && w.notches.size() >= 2 ? fc::notchPosition (w.notches, ps.get (w.param)) : fc::widgetPosition (w, *spec, ps.getNormalized (w.param));
        return w.invert != 0 ? 1.0 - t : t;
    }

    // Valeur normalisée du paramètre pour une position d'écran (0..1) : crans personnalisés, crans automatiques d'un curseur sur un
    // paramètre à pas, ou continue.
    double normalisedFor (const fc::Widget& w, double t, bool slider) const
    {
        t = std::clamp (w.invert != 0 ? 1.0 - t : t, 0.0, 1.0);
        const auto* spec = owner_.processor_.parameters().find (w.param);
        if (spec == nullptr) return t;
        if (w.stepMode == 2 && w.notches.size() >= 2) return spec->toNormalized (spec->clamp (fc::notchValueAt (w.notches, t)));
        const bool discrete = spec->kind == neptune::core::Kind::Choice || spec->kind == neptune::core::Kind::Stepped || spec->kind == neptune::core::Kind::Toggle;
        if (w.stepMode == 0 && slider && discrete && spec->max > spec->min) return spec->toNormalized (spec->min + std::round (t * (spec->max - spec->min)));
        return fc::widgetNormalised (w, *spec, t);
    }

    // Place la poignée du curseur sous le point (canevas) : crans automatiques sur un paramètre à pas ou à choix.
    void slideTo (const fc::Widget& w, juce::Point<float> p)
    {
        auto& o = owner_;
        const auto* spec = o.processor_.parameters().find (w.param);
        if (spec == nullptr) return;
        const juce::Image handle = w.style != 0 && o.facade_.findStyle (w.style) != nullptr ? picture (o.facade_.findStyle (w.style)->handle) : juce::Image();
        const float len = sliderHandleLength (w, handle);
        const bool vertical = w.vertical != 0;
        const float start = (vertical ? static_cast<float> (w.y) : static_cast<float> (w.x)) + len * 0.5f;
        const float travel = std::max (1.0f, static_cast<float> (vertical ? w.h : w.w) - len);
        double t = ((vertical ? p.y : p.x) - start) / travel;
        if (vertical) t = 1.0 - t;
        t = std::clamp (t, 0.0, 1.0);   // position d'écran ; normalisedFor applique l'inversion
        const double dragNorm = normalisedFor (w, t, true);
        o.processor_.uiSetNormalised (w.param, dragNorm);
        for (const auto id : w.mirror) o.processor_.uiSetNormalised (id, dragNorm);
        repaint();
    }

    void noteOn (const fc::Widget& w, int key)
    {
        owner_.processor_.uiKeyOn (owner_.track_, key, static_cast<float> (w.velocity) / 127.0f, w.channel - 1);
    }
    void noteOff (const fc::Widget& w, int key) { owner_.processor_.uiKeyOff (owner_.track_, key, w.channel - 1); }

    // Touche cliquée : la note part sur le canal du clavier. Maintien (réglage ou Maj) : une touche reste enfoncée jusqu'au clic suivant.
    // Molettes d'un clavier : hauteur (revient au milieu au lâcher) à gauche, modulation (reste où on la laisse) à droite ; messages MIDI sur le canal du clavier.
    static juce::Rectangle<float> wheelRect (const fc::Widget& w, int index)
    {
        return { static_cast<float> (w.x) + 4.0f + 28.0f * static_cast<float> (index), static_cast<float> (w.y) + 4.0f, 24.0f, static_cast<float> (w.h) - 8.0f };
    }
    void wheelSet (const fc::Widget& w, int index, float canvasY)
    {
        const auto r = wheelRect (w, index);
        const float capH = 14.0f, labelH = 12.0f;
        const float t = 1.0f - juce::jlimit (0.0f, 1.0f, (canvasY - capH * 0.5f - r.getY()) / std::max (1.0f, r.getHeight() - labelH - capH));
        sendWheel (w, index, index == 0 ? t * 2.0f - 1.0f : t);
    }
    void sendWheel (const fc::Widget& w, int index, float position)
    {
        wheelPos_[w.id][static_cast<std::size_t> (index)] = position;
        const int ch = juce::jlimit (0, 15, w.channel - 1);
        std::vector<std::uint8_t> bytes;
        int sent;
        if (index == 0)
        {
            sent = juce::jlimit (0, 16383, static_cast<int> (std::lround (8192.0f + position * 8191.0f)));
            bytes = { static_cast<std::uint8_t> (0xE0 | ch), static_cast<std::uint8_t> (sent & 127), static_cast<std::uint8_t> (sent >> 7) };
        }
        else
        {
            sent = juce::jlimit (0, 127, static_cast<int> (std::lround (position * 127.0f)));
            bytes = { static_cast<std::uint8_t> (0xB0 | ch), 1, static_cast<std::uint8_t> (sent) };
        }
        auto& last = wheelSent_[w.id];
        if (last[static_cast<std::size_t> (index)] != sent || index == 0)
        {
            last[static_cast<std::size_t> (index)] = sent;
            owner_.processor_.postMidi (bytes, 1);
        }
        repaint();
    }

    void keyDown (const juce::MouseEvent& e, const fc::Widget& w)
    {
        if (w.wheels != 0 && toCanvas (e.position).x < static_cast<float> (w.x + fc::keyboardWheelsWidth))
        {
            mode_ = Mode::Wheel;
            wheelWidget_ = w.id;
            wheelIndex_ = toCanvas (e.position).x < static_cast<float> (w.x) + 30.0f ? 0 : 1;
            wheelSet (w, wheelIndex_, toCanvas (e.position).y);
            return;
        }
        const int key = keyAtPoint (w, toCanvas (e.position));
        if (key < 0) return;
        if (w.hold != 0 || e.mods.isShiftDown())
        {
            auto& keys = latched_[w.id];
            if (keys.erase (key) != 0) noteOff (w, key);
            else { keys.insert (key); noteOn (w, key); }
        }
        else
        {
            pressed_[e.source.getIndex()] = { w.id, key };
            mode_ = Mode::Key;
            noteOn (w, key);
        }
        repaint();
    }

    bool keyIsDown (int widgetId, int key) const
    {
        for (const auto& kv : pressed_)
            if (kv.second.first == widgetId && kv.second.second == key) return true;
        const auto it = latched_.find (widgetId);
        return it != latched_.end() && it->second.count (key) != 0;
    }

    bool topAligned_ = false;

    float scale() const
    {
        return std::min (static_cast<float> (getWidth()) / fc::canvasWidth, static_cast<float> (getHeight()) / fc::canvasHeight);
    }
    juce::Rectangle<float> canvasArea() const
    {
        const float s = scale();
        auto r = juce::Rectangle<float> (fc::canvasWidth * s, fc::canvasHeight * s).withCentre (getLocalBounds().toFloat().getCentre());
        return r;
    }
    juce::Point<float> toCanvas (juce::Point<float> p) const
    {
        const auto a = canvasArea();
        const float s = scale();
        return { (p.x - a.getX()) / s, (p.y - a.getY()) / s };
    }

    const fc::Widget* widgetAt (juce::Point<float> p) const
    {
        const auto& ws = owner_.facade_.widgets;
        for (const bool zones : { false, true })   // le contenu d'une zone prime sur la zone
            for (auto it = ws.rbegin(); it != ws.rend(); ++it)
                if ((it->type == fc::WidgetType::Zone) == zones
                    && juce::Rectangle<float> (static_cast<float> (it->x), static_cast<float> (it->y), static_cast<float> (it->w), static_cast<float> (it->h)).contains (p))
                    return &*it;
        return nullptr;
    }

    std::set<int> movingSet() const
    {
        auto ids = owner_.selection_;
        for (const int id : owner_.selection_) ids.merge (fc::descendants (owner_.facade_, id));
        return ids;
    }

    void editDown (juce::Point<float> p, const juce::ModifierKeys& mods)
    {
        auto& o = owner_;
        before_ = o.facade_;
        dragStart_ = p;
        const bool extend = mods.isShiftDown() || mods.isCommandDown();
        if (const auto* sel = o.facade_.find (o.single()))
        {
            const juce::Rectangle<float> handle (static_cast<float> (sel->x + sel->w) - 14.0f, static_cast<float> (sel->y + sel->h) - 14.0f, 14.0f, 14.0f);
            if (handle.contains (p))
            {
                mode_ = Mode::Resize;
                startRect_ = { sel->x, sel->y, sel->w, sel->h };
                return;
            }
        }
        const auto* w = widgetAt (p);
        if (w != nullptr)
        {
            o.lastClicked_ = w->id;
            if (extend)
            {
                if (o.selection_.count (w->id) != 0) o.selection_.erase (w->id); else o.selection_.insert (w->id);
            }
            else if (o.selection_.count (w->id) == 0)
                o.selection_ = { w->id };
            if (o.selection_.count (w->id) != 0)
            {
                mode_ = Mode::Move;
                moving_ = movingSet();
            }
        }
        else
        {
            if (! extend) o.selection_.clear();
            bandBase_ = o.selection_;
            band_ = {};
            mode_ = Mode::Band;
        }
        o.updateButtons();
        repaint();
    }

    void toggle (neptune::core::ParamId id, const std::vector<neptune::core::ParamId>& mirror = {})
    {
        auto& o = owner_;
        auto& ps = o.processor_.parameters();
        const auto* spec = ps.find (id);
        if (spec == nullptr) return;
        const double plain = ps.get (id);
        double next;
        if ((spec->kind == neptune::core::Kind::Choice || spec->kind == neptune::core::Kind::Stepped) && spec->max - spec->min > 1.0)
            next = plain + 1.0 > spec->max ? spec->min : plain + 1.0;   // plusieurs positions : on passe à la suivante
        else
            next = ps.getNormalized (id) >= 0.5 ? spec->min : spec->max;
        o.processor_.uiBeginEdit (id);
        o.processor_.uiSetNormalised (id, spec->toNormalized (next));
        for (const auto other : mirror) o.processor_.uiSetNormalised (other, spec->toNormalized (next));   // mêmes bornes : un réglage partagé entre claviers
        o.processor_.uiEndEdit (id);
    }

    // Menu déroulant : toutes les valeurs du paramètre.
    void showValueMenu (const fc::Widget& w)
    {
        auto& o = owner_;
        auto& ps = o.processor_.parameters();
        const auto* spec = ps.find (w.param);
        if (spec == nullptr) return;
        const int count = menuCount (*spec);
        if (count <= 0) return;
        const int current = static_cast<int> (std::lround (ps.get (w.param) - spec->min));
        juce::PopupMenu menu;
        for (int i = 0; i < count; ++i)
            menu.addItem (i + 1, utf8 (ps.toText (*spec, spec->min + i)), true, i == current);
        menu.setLookAndFeel (&getLookAndFeel());
        const auto id = w.param;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (widgetScreenRect (w))),
                            [safe = juce::Component::SafePointer<Canvas> (this), id] (int choice)
                            {
                                if (safe == nullptr || choice <= 0) return;
                                auto& proc = safe->owner_.processor_;
                                const auto* sp = proc.parameters().find (id);
                                if (sp == nullptr) return;
                                proc.uiBeginEdit (id);
                                proc.uiSetNormalised (id, sp->toNormalized (sp->min + (choice - 1)));
                                proc.uiEndEdit (id);
                            });
    }

    juce::Rectangle<int> widgetScreenRect (const fc::Widget& w) const
    {
        const auto a = canvasArea();
        const float s = scale();
        return juce::Rectangle<float> (a.getX() + w.x * s, a.getY() + w.y * s, w.w * s, w.h * s).toNearestInt();
    }

    // Bouton d'action : toutes les valeurs de l'instantané d'un coup (un seul geste, donc une seule étape d'annulation).
    void runAction (const fc::Widget& w)
    {
        auto& o = owner_;
        auto& ps = o.processor_.parameters();
        if (w.snapshot.empty()) return;
        o.processor_.uiBeginEdit (w.snapshot.front().first);
        for (const auto& [id, value] : w.snapshot)
            if (const auto* sp = ps.find (id)) o.processor_.uiSetNormalised (id, sp->toNormalized (sp->clamp (w.actionMode == 1 ? sp->defaultValue : value)));   // mode 1 : annulation générale
        o.processor_.uiEndEdit (w.snapshot.front().first);
    }

    // Piston : mémorise les valeurs courantes de sa liste de paramètres (un seul geste annulable), puis désarme « Set ».
    void storePiston (int id)
    {
        auto& o = owner_;
        auto f = o.facade_;
        auto& ps = o.processor_.parameters();
        if (auto* w = f.find (id))
        {
            for (auto& entry : w->snapshot)
                if (ps.find (entry.first) != nullptr) entry.second = ps.get (entry.first);
            o.commit (std::move (f), "Facade: store piston");
        }
        o.processor_.uiCombiArm (false);
        repaint();
    }

    void timerCallback() override
    {
        stopTimer();
        if (pistonId_ == 0) return;
        longPress_ = true;
        storePiston (pistonId_);
    }

    // Piste visée par un divisionnel : celle de la Façade, ou la piste sélectionnée pour la Façade globale.
    int combiTrack() const { return owner_.global_ ? owner_.processor_.selectedTrack() : owner_.track_; }

    // Grille de scènes (jeu) : flèches de banque, SET, un clic sur une case rappelle la scène (ou l'enregistre si SET est armé), clic droit : texte ou effacement.
    void sceneGridDown (const juce::MouseEvent& e, const fc::Widget& w)
    {
        namespace cb = neptune::core::combi;
        auto& o = owner_;
        const auto p = toCanvas (e.position);
        const auto geo = sceneGridGeometry (w);
        if (w.header != 0 && ! e.mods.isPopupMenu())
        {
            // les flèches de banque demandent un appui long (un doigt qui frôle l'écran en concert ne change pas de banque) ; maintenu, le défilement continue
            if (geo.prev.contains (p) || geo.next.contains (p))
            {
                bankDir_ = geo.prev.contains (p) ? -1 : 1;
                bankHoldId_ = w.id;
                bankHoldStartMs_ = juce::Time::getMillisecondCounterHiRes();
                bankHold_.startTimer (30);
                repaint();
                return;
            }
            if (w.setPill != 0 && geo.setPill.contains (p)) { o.processor_.uiCombiArm (! o.processor_.combiArmed()); repaint(); return; }
        }
        for (std::size_t i = 0; i < geo.cells.size(); ++i)
        {
            if (! geo.cells[i].contains (p)) continue;
            const int piston = static_cast<int> (i);
            if (! e.mods.isPopupMenu()) { o.processor_.uiCombiScene (piston); repaint(); return; }
            juce::PopupMenu menu;
            menu.addItem (1, o.text ("facade.grid_text"));
            menu.addItem (2, o.text ("facade.grid_erase"));
            menu.setLookAndFeel (&o.getLookAndFeel());
            menu.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<FacadeView> (&o), piston] (int chosen)
            {
                if (safe == nullptr || chosen <= 0) return;
                if (chosen == 1) { safe->editSceneText (piston); return; }
                auto lib = safe->processor_.combinations();
                if (cb::eraseGeneral (lib, cb::generalKey (safe->processor_.combiStatus().bank, piston))) safe->processor_.uiEditCombinations (std::move (lib), "Scene: erase");
            });
            return;
        }
    }

    // Texte d'un bouton de combinaison : « G3 », « D3 », « GC », « Set », et la banque ou le pas courant pour les boutons qui les changent.
    juce::String combiCaption (int command) const
    {
        namespace cb = neptune::core::combi;
        juce::String label = juce::String::fromUTF8 (cb::commandLabel (command).c_str());
        if (command == cb::kCommandBankNext || command == cb::kCommandBankPrevious)
            label << " (" << (owner_.processor_.combiStatus().bank + 1) << ")";
        else if (command == cb::kCommandNext || command == cb::kCommandPrevious)
        {
            const auto status = owner_.processor_.combiStatus();
            if (status.sequenceLength > 0) label << " (" << (status.sequencePosition + 1) << "/" << status.sequenceLength << ")";
        }
        return label;
    }

    bool actionIsLit (const fc::Widget& w) const
    {
        if (w.actionMode >= 2)
        {
            const int command = w.actionMode - 1;
            return command == neptune::core::combi::kCommandSet ? owner_.processor_.combiArmed() : owner_.processor_.combiLit (command, combiTrack());
        }
        auto& ps = owner_.processor_.parameters();
        if (w.snapshot.empty()) return false;
        for (const auto& [id, value] : w.snapshot)
        {
            const auto* sp = ps.find (id);
            if (sp == nullptr || std::abs (ps.get (id) - sp->clamp (w.actionMode == 1 ? sp->defaultValue : value)) > 1.0e-6) return false;
        }
        return true;
    }

    // Image du cache (chargée au premier besoin) ; vide si absente ou illisible. L'échec est mémorisé : pas de relecture à chaque image.
    juce::Image picture (const std::string& path, int maxSide = 1024) const
    {
        if (path.empty()) return {};
        const auto it = pictures_.find (path);
        if (it != pictures_.end()) return it->second.image;
        auto& entry = pictures_[path];
        entry.image = loadPicture (path, maxSide, svgVars_, entry.natural);
        return entry.image;
    }

    juce::Point<float> naturalSize (const std::string& path) const
    {
        picture (path);
        const auto it = pictures_.find (path);
        return it != pictures_.end() ? it->second.natural : juce::Point<float>();
    }

    void paintWidget (juce::Graphics& g, const fc::Widget& w) const
    {
        auto& o = owner_;
        auto& ps = o.processor_.parameters();
        const auto* spec = w.param != 0 ? ps.find (w.param) : nullptr;
        const bool linked = spec != nullptr;
        const juce::Rectangle<float> b (static_cast<float> (w.x), static_cast<float> (w.y), static_cast<float> (w.w), static_cast<float> (w.h));
        const juce::String caption0 = w.text.empty() ? (linked ? utf8 (spec->name) : juce::String()) : utf8 (w.text);
        const bool isCombi = w.type == fc::WidgetType::Action && w.actionMode >= 2;
        const juce::String caption = w.type == fc::WidgetType::Set && w.text.empty() ? juce::String ("Set") : isCombi && w.text.empty() ? combiCaption (w.actionMode - 1) : caption0;
        const float n = linked ? static_cast<float> (ps.getNormalized (w.param)) : 0.0f;
        const float pos = linked ? static_cast<float> (shownPosition (w)) : 0.0f;   // potard et curseur : crans et inversion compris

        switch (w.type)
        {
            case fc::WidgetType::Knob:
            {
                auto r = b;
                auto valueRow = r.removeFromBottom (15.0f);
                auto captionRow = r.removeFromBottom (17.0f);
                const auto* style = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const juce::Image pointer = style != nullptr ? picture (style->pointer) : juce::Image();
                const juce::Image strip = style != nullptr && style->frames > 1 ? picture (style->strip, 4096) : juce::Image();
                if (pointer.isValid())
                {
                    // deux couches : un corps fixe et une aiguille que le code fait tourner
                    const juce::Image body = picture (style->body);
                    const auto& ref = body.isValid() ? body : pointer;
                    const auto dest = juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (ref.getWidth()), static_cast<float> (ref.getHeight())), r);
                    if (body.isValid()) g.drawImage (body, dest, juce::RectanglePlacement::stretchToFit);
                    const float degrees = static_cast<float> (style->angleMin) + pos * static_cast<float> (style->angleMax - style->angleMin);
                    juce::Graphics::ScopedSaveState keep (g);
                    g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (degrees), dest.getX() + dest.getWidth() * static_cast<float> (style->pivotX) / 100.0f,
                                                                      dest.getY() + dest.getHeight() * static_cast<float> (style->pivotY) / 100.0f));
                    g.drawImage (pointer, dest, juce::RectanglePlacement::stretchToFit);
                }
                else if (strip.isValid())
                {
                    // bande d'images : une image par position du potard
                    const int frames = style->frames, frame = juce::jlimit (0, frames - 1, juce::roundToInt (pos * static_cast<float> (frames - 1)));
                    const int fw = style->stripHorizontal ? strip.getWidth() / frames : strip.getWidth();
                    const int fh = style->stripHorizontal ? strip.getHeight() : strip.getHeight() / frames;
                    if (fw > 0 && fh > 0)
                    {
                        const auto dest = juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (fw), static_cast<float> (fh)), r).toNearestInt();
                        g.drawImage (strip, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(),
                                     style->stripHorizontal ? frame * fw : 0, style->stripHorizontal ? 0 : frame * fh, fw, fh);
                    }
                }
                else
                    paintNeptuneKnob (g, r, pos, o.palette_, o.knobStyle_);
                if (linked)
                    if (const float off = ps.modOffsetAt (ps.indexOf (w.param)); off != 0.0f)
                    {
                        // la position modulée suit l'échelle et la courbe du widget : on la recalcule à partir de la valeur normalisée modulée
                        const auto* modSpec = ps.find (w.param);
                        const double modulated = modSpec != nullptr ? fc::widgetPosition (w, *modSpec, std::clamp (ps.getNormalized (w.param) + static_cast<double> (off), 0.0, 1.0)) : pos + off;
                        paintModulationRing (g, r, pos, static_cast<float> (w.invert != 0 ? 1.0 - modulated : modulated), o.palette_);
                    }
                if (style != nullptr && (style->textSize > 0 || ! style->textColour.empty() || style->textPosition != 0 || ! style->textBold))
                    drawStyledText (g, style, caption, captionRow, 0.8f, o.colour (Token::Text));
                else
                {
                    g.setColour (o.colour (Token::Text));
                    g.setFont (juce::FontOptions (14.0f));
                    g.drawFittedText (caption, captionRow.toNearestInt(), juce::Justification::centred, 1);
                }
                g.setColour (o.colour (Token::Accent));
                g.setFont (juce::FontOptions (13.0f));
                g.drawFittedText (linked ? utf8 (ps.toText (w.param)) : (o.editMode_ ? o.text ("facade.unlinked") : juce::String()), valueRow.toNearestInt(), juce::Justification::centred, 1);
                break;
            }
            case fc::WidgetType::Button:
            case fc::WidgetType::Action:
            case fc::WidgetType::Set:
            {
                const bool isAction = w.type == fc::WidgetType::Action;
                const bool isSet = w.type == fc::WidgetType::Set;
                const bool on = isSet ? o.processor_.combiArmed() : isAction ? actionIsLit (w) : linked && n >= 0.5f;
                const bool empty = isSet ? false : isAction ? (w.actionMode < 2 && w.snapshot.empty()) : ! linked;
                if (const auto* style = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr)
                {
                    const bool hover = hoverId_ == w.id && ! o.editMode_;
                    const std::string* path = &(on ? style->on : style->off);
                    if (hover && ! (on ? style->onHover : style->offHover).empty()) path = &(on ? style->onHover : style->offHover);
                    if (empty && ! o.editMode_ && ! style->disabled.empty()) path = &style->disabled;   // widget non lié, en mode Jeu
                    const juce::Image face = picture (*path);
                    if (face.isValid())
                    {
                        g.drawImage (face, b, juce::RectanglePlacement::centred);
                        drawStyledText (g, style, isCombi ? caption : utf8 (w.text), b, 0.3f, o.colour (Token::Text));
                        break;
                    }
                }
                const auto r = b.reduced (1.0f);
                g.setColour (on ? o.colour (Token::Accent) : o.colour (Token::Pb));
                g.fillRoundedRectangle (r, 8.0f);
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (r, 8.0f, 1.5f);
                g.setColour (on ? o.colour (Token::BgApp) : o.colour (Token::Text));
                g.setFont (juce::FontOptions (juce::jlimit (11.0f, 24.0f, b.getHeight() * 0.42f), juce::Font::bold));
                g.drawFittedText (caption.isEmpty() && empty && o.editMode_ ? o.text ("facade.unlinked") : caption, r.toNearestInt().reduced (6, 2), juce::Justification::centred, 2);
                break;
            }
            case fc::WidgetType::Menu:
            {
                auto r = b.reduced (1.0f);
                auto captionRow = r.removeFromTop (std::min (18.0f, r.getHeight() * 0.4f));
                g.setColour (o.colour (Token::TextDim));
                g.setFont (juce::FontOptions (13.0f));
                g.drawFittedText (caption, captionRow.toNearestInt(), juce::Justification::centredLeft, 1);
                g.setColour (o.colour (Token::Pb));
                g.fillRoundedRectangle (r, 6.0f);
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (r, 6.0f, 1.5f);
                g.setColour (o.colour (Token::Text));
                g.setFont (juce::FontOptions (juce::jlimit (11.0f, 22.0f, r.getHeight() * 0.5f)));
                g.drawFittedText (linked ? utf8 (ps.toText (w.param)) : (o.editMode_ ? o.text ("facade.unlinked") : juce::String()),
                                  r.reduced (8.0f, 2.0f).withTrimmedRight (18.0f).toNearestInt(), juce::Justification::centredLeft, 1);
                juce::Path arrow;
                const float cx = r.getRight() - 14.0f, cy = r.getCentreY();
                arrow.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
                g.setColour (o.colour (Token::Accent));
                g.fillPath (arrow);
                break;
            }
            case fc::WidgetType::Curve:
            {
                auto body = b;
                if (! w.text.empty())
                {
                    g.setColour (o.colour (Token::Text));
                    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
                    g.drawFittedText (utf8 (w.text), body.removeFromTop (curveTitleHeight).toNearestInt(), juce::Justification::centredLeft, 1);
                }
                if (! o.editMode_) break;   // le panneau réel est posé par-dessus
                g.setColour (o.colour (Token::BgApp));
                g.fillRoundedRectangle (body.reduced (1.0f), 8.0f);
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (body.reduced (1.0f), 8.0f, 1.5f);
                g.setColour (o.colour (Token::TextDim));
                g.setFont (juce::FontOptions (16.0f));
                g.drawFittedText (o.text ("facade.curve_title"), body.toNearestInt().reduced (12), juce::Justification::centred, 3);
                break;
            }
            case fc::WidgetType::Slider:
            {
                const auto* style = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const juce::Image rail = style != nullptr ? picture (style->track) : juce::Image();
                const juce::Image handle = style != nullptr ? picture (style->handle) : juce::Image();
                const bool vertical = w.vertical != 0;
                if (rail.isValid()) g.drawImage (rail, b, juce::RectanglePlacement::stretchToFit);
                else
                {
                    g.setColour (o.colour (Token::Border));
                    if (vertical) g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, b.getHeight()).withCentre (b.getCentre()), 2.0f);
                    else g.fillRoundedRectangle (juce::Rectangle<float> (b.getWidth(), 4.0f).withCentre (b.getCentre()), 2.0f);
                }
                const float len = sliderHandleLength (w, handle);
                const float travel = (vertical ? b.getHeight() : b.getWidth()) - len;
                juce::Rectangle<float> hr = vertical ? juce::Rectangle<float> (b.getX(), b.getBottom() - len - pos * travel, b.getWidth(), len)
                                                     : juce::Rectangle<float> (b.getX() + pos * travel, b.getY(), len, b.getHeight());
                if (handle.isValid()) g.drawImage (handle, hr, juce::RectanglePlacement::centred);
                else
                {
                    g.setColour (linked ? o.colour (Token::Accent) : o.colour (Token::TextDim2));
                    g.fillRoundedRectangle (hr.reduced (1.0f), 4.0f);
                    g.setColour (o.colour (Token::Text));
                    g.drawRoundedRectangle (hr.reduced (1.0f), 4.0f, 1.0f);
                }
                if (! w.text.empty())
                {
                    if (style != nullptr) drawStyledText (g, style, utf8 (w.text), b, 0.3f, o.colour (Token::TextDim), juce::Justification::centredBottom);
                    else
                    {
                        g.setColour (o.colour (Token::TextDim));
                        g.setFont (juce::FontOptions (12.0f));
                        g.drawFittedText (utf8 (w.text), b.toNearestInt(), juce::Justification::centredBottom, 1);
                    }
                }
                else if (! linked && o.editMode_)
                {
                    g.setColour (o.colour (Token::TextDim));
                    g.setFont (juce::FontOptions (12.0f));
                    g.drawFittedText (o.text ("facade.unlinked"), b.toNearestInt(), juce::Justification::centred, 1);
                }
                break;
            }
            case fc::WidgetType::Meter:
            {
                const bool vertical = w.vertical != 0;
                const auto it = meters_.find (w.id);
                const MeterState state = it != meters_.end() ? it->second : MeterState { { -100.0f, -100.0f }, { -100.0f, -100.0f }, {}, };
                const bool reduction = fc::decodeMeterSource (w.source).reduction;
                auto area = b.reduced (2.0f);
                g.setColour (o.colour (Token::BgApp));
                g.fillRoundedRectangle (b, 4.0f);
                const float gap = 3.0f;
                auto place = [&] (int channel) -> juce::Rectangle<float>
                {
                    const float half = ((vertical ? area.getWidth() : area.getHeight()) - gap) / 2.0f;
                    return vertical ? juce::Rectangle<float> (area.getX() + channel * (half + gap), area.getY(), half, area.getHeight())
                                    : juce::Rectangle<float> (area.getX(), area.getY() + channel * (half + gap), area.getWidth(), half);
                };
                auto fraction = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };   // -60 dB .. 0 dB
                for (int i = 0; i < 2; ++i)
                {
                    const auto bar = place (i);
                    g.setColour (o.colour (Token::Pb));
                    g.fillRect (bar);
                    // le niveau : de l'accent au rouge au-dessus de -6 dB
                    for (const bool hot : { false, true })
                    {
                        const float lo = hot ? fraction (-6.0f) : 0.0f, hi = std::min (fraction (state.level[i]), hot || reduction ? 1.0f : fraction (-6.0f));
                        if (reduction && hot) continue;   // la réduction de gain garde la couleur d'accent
                        if (hi <= lo) continue;
                        g.setColour (hot ? o.colour (Token::KnobHot) : o.colour (Token::Accent));
                        if (vertical) g.fillRect (juce::Rectangle<float> (bar.getX(), bar.getBottom() - hi * bar.getHeight(), bar.getWidth(), (hi - lo) * bar.getHeight()));
                        else g.fillRect (juce::Rectangle<float> (bar.getX() + lo * bar.getWidth(), bar.getY(), (hi - lo) * bar.getWidth(), bar.getHeight()));
                    }
                    // la crête maintenue
                    g.setColour (o.colour (Token::Text));
                    const float hp = fraction (state.hold[i]);
                    if (hp > 0.0f)
                    {
                        if (vertical) g.fillRect (bar.getX(), bar.getBottom() - hp * bar.getHeight(), bar.getWidth(), 2.0f);
                        else g.fillRect (bar.getX() + hp * bar.getWidth() - 2.0f, bar.getY(), 2.0f, bar.getHeight());
                    }
                }
                // graduations tous les 12 dB
                g.setColour (o.colour (Token::TextDim2));
                const float marks[5] = { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f };   // niveau : tous les 12 dB ; réduction : les mêmes traits valent 6 dB de gain
                for (const float mark : marks)
                {
                    const float f = fraction (mark);
                    if (vertical) g.drawHorizontalLine (juce::roundToInt (area.getBottom() - f * area.getHeight()), area.getX(), area.getRight());
                    else g.drawVerticalLine (juce::roundToInt (area.getX() + f * area.getWidth()), area.getY(), area.getBottom());
                }
                if (! w.text.empty()) drawStyledText (g, nullptr, utf8 (w.text), b, 0.12f, o.colour (Token::TextDim), juce::Justification::centredTop);
                break;
            }
            case fc::WidgetType::SceneGrid:
            {
                namespace cb = neptune::core::combi;
                const auto geo = sceneGridGeometry (w);
                const auto status = o.processor_.combiStatus();
                const auto scenes = o.processor_.sceneCells (status.bank, static_cast<int> (geo.cells.size()));
                const int lit = status.lastKey >= 0 && status.lastKey / cb::kPistons == status.bank ? status.lastKey % cb::kPistons : -1;
                g.setColour (o.colour (Token::BgApp));
                g.fillRoundedRectangle (b, 8.0f);
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (b.reduced (0.5f), 8.0f, 1.0f);
                if (w.header != 0)
                {
                    auto arrow = [&] (juce::Rectangle<float> r, bool left)
                    {
                        const bool held = bankDir_ == (left ? -1 : 1) && bankHoldId_ == w.id;
                        g.setColour (o.colour (Token::Pb));
                        g.fillRoundedRectangle (r, 6.0f);
                        if (held)   // une barre qui se remplit pendant l'appui : la banque change quand elle est pleine
                        {
                            const float progress = static_cast<float> (juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - bankHoldStartMs_) / 500.0));
                            juce::Graphics::ScopedSaveState keep (g);
                            juce::Path shape;
                            shape.addRoundedRectangle (r, 6.0f);
                            g.reduceClipRegion (shape);
                            g.setColour (o.colour (Token::Accent));
                            g.fillRect (r.withWidth (r.getWidth() * progress));
                        }
                        g.setColour (o.colour (Token::Text));
                        juce::Path tri;
                        const auto c = r.getCentre();
                        if (left) tri.addTriangle (c.x + 6.0f, c.y - 8.0f, c.x + 6.0f, c.y + 8.0f, c.x - 7.0f, c.y); else tri.addTriangle (c.x - 6.0f, c.y - 8.0f, c.x - 6.0f, c.y + 8.0f, c.x + 7.0f, c.y);
                        g.fillPath (tri);
                    };
                    arrow (geo.prev, true);
                    arrow (geo.next, false);
                    g.setColour (o.colour (Token::Text));
                    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
                    g.drawText (juce::String::fromUTF8 (o.tr_.format ("facade.grid_bank", { std::to_string (status.bank + 1), std::to_string (cb::kBanks) }).c_str()), geo.label.withTrimmedBottom (9.0f), juce::Justification::centred);
                    g.setColour (o.colour (Token::TextDim2));
                    g.setFont (juce::FontOptions (10.0f));
                    g.drawText (o.text ("facade.grid_long_press"), geo.label.withTrimmedTop (22.0f), juce::Justification::centredTop);
                    if (w.setPill != 0)
                    {
                        const bool armed = status.setArmed;
                        g.setColour (armed ? o.colour (Token::KnobHot) : o.colour (Token::Pb));
                        g.fillRoundedRectangle (geo.setPill, 6.0f);
                        g.setColour (armed ? juce::Colours::white : o.colour (Token::TextDim));
                        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
                        g.drawText ("SET", geo.setPill, juce::Justification::centred);
                    }
                }
                for (std::size_t i = 0; i < geo.cells.size(); ++i)
                {
                    const auto c = geo.cells[i];
                    const bool used = i < scenes.size() && scenes[i].used;
                    const bool isLit = static_cast<int> (i) == lit && used;
                    const auto fill = used ? sceneColour (w, isLit) : o.colour (Token::BgCard);
                    g.setColour (fill);
                    g.fillRoundedRectangle (c, 8.0f);
                    g.setColour (isLit ? juce::Colours::white : (used ? fill.brighter (0.25f) : o.colour (Token::Border)));
                    g.drawRoundedRectangle (c.reduced (0.5f), 8.0f, isLit ? 3.0f : 1.0f);
                    const auto ink = used ? readableOn (fill) : o.colour (Token::TextDim2);
                    auto inner = c.reduced (6.0f, 3.0f);
                    g.setColour (ink.withAlpha (0.65f));
                    g.setFont (juce::FontOptions (juce::jlimit (10.0f, 14.0f, c.getHeight() * 0.16f)));
                    g.drawText (juce::String (static_cast<int> (i) + 1), inner.withHeight (c.getHeight() * 0.18f), juce::Justification::topLeft);   // le numéro, dans le coin
                    if (! used) continue;
                    const auto& sc = scenes[i];
                    // le nom commence tout en haut de la case, à la hauteur du numéro ; une ou deux lignes de texte dessous
                    const int lines = (sc.note.empty() ? 0 : 1) + (sc.note2.empty() ? 0 : 1);
                    const float noteScale = static_cast<float> (w.noteSize) / 100.0f, nameScale = static_cast<float> (w.nameSize) / 100.0f;
                    const float lineH = lines > 0 ? std::min (inner.getHeight() * 0.2f * noteScale, inner.getHeight() * 0.56f / static_cast<float> (lines)) : 0.0f;
                    auto notesArea = lines > 0 ? inner.removeFromBottom (lineH * static_cast<float> (lines)) : juce::Rectangle<float>();
                    auto nameArea = inner.reduced (std::min (18.0f, inner.getWidth() * 0.12f), 0.0f);   // à l'écart du numéro, en haut à gauche
                    drawAutoFit (g, utf8 (sc.name), lines > 0 ? nameArea : nameArea.withTrimmedTop (0.0f), c.getHeight() * 0.2f * nameScale, true, 2, true, ink);
                    for (const auto* line : { &sc.note, &sc.note2 })
                        if (! line->empty()) drawAutoFit (g, utf8 (*line), notesArea.removeFromTop (lineH), lineH * 0.8f, false, 1, false, ink.withAlpha (0.88f));
                }
                break;
            }
            case fc::WidgetType::Midi:
            {
                const bool pressed = midiId_ == w.id;
                const auto* style = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const juce::String label = w.text.empty() ? juce::String::fromUTF8 (neptune::core::midisend::describe (w.midi).c_str()) : utf8 (w.text);
                const juce::Image face = style != nullptr && w.midiFader == 0 ? picture (pressed ? style->on : style->off) : juce::Image();
                if (face.isValid())
                {
                    g.drawImage (face, b, juce::RectanglePlacement::centred);
                    drawStyledText (g, style, utf8 (w.text), b, 0.3f, o.colour (Token::Text));
                    break;
                }
                const auto r = b.reduced (1.0f);
                g.setColour (o.colour (Token::Pb));
                g.fillRoundedRectangle (r, 8.0f);
                if (w.midiFader != 0)
                {
                    const auto it = midiFader_.find (w.id);
                    const double t = it != midiFader_.end() ? it->second : 0.0;
                    g.setColour (o.colour (Token::Accent).withAlpha (0.55f));
                    g.fillRoundedRectangle (r.withWidth (static_cast<float> (r.getWidth() * t)), 8.0f);
                }
                else if (pressed) { g.setColour (o.colour (Token::Accent)); g.fillRoundedRectangle (r, 8.0f); }
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (r, 8.0f, 1.5f);
                g.setColour (pressed && w.midiFader == 0 ? o.colour (Token::BgApp) : o.colour (Token::Text));
                g.setFont (juce::FontOptions (juce::jlimit (11.0f, 22.0f, b.getHeight() * 0.38f), juce::Font::bold));
                g.drawFittedText (label, r.toNearestInt().reduced (6, 2), juce::Justification::centred, 2);
                break;
            }
            case fc::WidgetType::Lamp:
            {
                bool on = false;
                if (w.source == 0) on = linked && n >= 0.5f;
                else
                {
                    if (w.source == 1) on = (o.processor_.heldChannelWordAny (w.channel - 1, 0) | o.processor_.heldChannelWordAny (w.channel - 1, 1)) != 0;   // le canal, quelle que soit la piste qui le joue
                    else if (o.track_ >= 0) on = (o.processor_.heldKeyWord (o.track_, 0) | o.processor_.heldKeyWord (o.track_, 1)) != 0;
                }
                const auto* style = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const bool unlinked = w.source == 0 && ! linked;
                const std::string* path = style != nullptr ? &(on ? style->on : style->off) : nullptr;
                if (style != nullptr && unlinked && ! o.editMode_ && ! style->disabled.empty()) path = &style->disabled;
                const juce::Image face = path != nullptr ? picture (*path) : juce::Image();
                if (face.isValid()) g.drawImage (face, b, juce::RectanglePlacement::centred);
                else
                {
                    const auto c = b.reduced (3.0f).withSizeKeepingCentre (std::min (b.getWidth(), b.getHeight()) - 6.0f, std::min (b.getWidth(), b.getHeight()) - 6.0f);
                    if (on) { g.setColour (o.colour (Token::Accent).withAlpha (0.25f)); g.fillEllipse (c.expanded (3.0f)); }
                    g.setColour (on ? o.colour (Token::Accent) : o.colour (Token::BgApp));
                    g.fillEllipse (c);
                    g.setColour (o.colour (Token::Border));
                    g.drawEllipse (c, 1.5f);
                }
                if (! w.text.empty()) drawStyledText (g, style, utf8 (w.text), b, 0.3f, o.colour (Token::Text), juce::Justification::centredBottom);
                else if (unlinked && o.editMode_) { g.setColour (o.colour (Token::TextDim)); g.setFont (juce::FontOptions (11.0f)); g.drawFittedText (o.text ("facade.unlinked"), b.toNearestInt(), juce::Justification::centred, 1); }
                break;
            }
            case fc::WidgetType::Keyboard:
            {
                juce::Graphics::ScopedSaveState keep (g);
                g.reduceClipRegion (b.toNearestInt());
                g.setColour (o.colour (Token::BgApp));
                g.fillRect (b);
                const auto* kstyle = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const juce::Image imgs[4] = { kstyle != nullptr ? picture (kstyle->keyWhite) : juce::Image(), kstyle != nullptr ? picture (kstyle->keyWhiteOn) : juce::Image(),
                                              kstyle != nullptr ? picture (kstyle->keyBlack) : juce::Image(), kstyle != nullptr ? picture (kstyle->keyBlackOn) : juce::Image() };
                const auto litColour = kstyle != nullptr && ! kstyle->keyLit.empty() ? parseColour (kstyle->keyLit, o.colour (Token::Accent)) : o.colour (Token::Accent);
                std::uint64_t held[2] = { 0, 0 };
                {
                    held[0] = o.processor_.heldChannelWordAny (w.channel - 1, 0);   // les notes du canal, quelle que soit la piste qui les joue
                    held[1] = o.processor_.heldChannelWordAny (w.channel - 1, 1);
                }
                g.saveState();
                g.reduceClipRegion ((w.wheels != 0 ? b.withTrimmedLeft (static_cast<float> (fc::keyboardWheelsWidth)) : b).toNearestInt());   // les touches ne passent pas sous les molettes
                for (const bool black : { false, true })
                    for (int k = 0; k < 128; ++k)
                    {
                        if (keyIsBlack (k) != black) continue;
                        const auto r = keyRect (w, k);
                        if (r.getRight() < b.getX() || r.getX() > b.getRight()) continue;
                        const bool down = keyIsDown (w.id, k), midi = ((held[k >> 6] >> (k & 63)) & 1u) != 0;
                        const bool lit = down || midi;   // l'image « enfoncée » sert aussi aux notes jouées de l'extérieur
                        const auto& rest = imgs[black ? 2 : 0];
                        const auto& pressed = imgs[black ? 3 : 1];
                        if (lit && pressed.isValid())
                            g.drawImage (pressed, r, juce::RectanglePlacement::stretchToFit);
                        else if (rest.isValid())
                        {
                            g.drawImage (rest, r, juce::RectanglePlacement::stretchToFit);
                            if (lit) { g.setColour (litColour.withAlpha (0.5f)); g.fillRect (r.reduced (0.5f)); }   // pas d'image « enfoncée » : teinte par-dessus
                        }
                        else
                        {
                            g.setColour (down ? litColour : midi ? litColour.withAlpha (0.7f) : black ? juce::Colour (0xff1a1a1a) : juce::Colour (0xfff2f2f2));
                            g.fillRect (r.reduced (0.5f));
                            g.setColour (juce::Colour (0xff555555));
                            g.drawRect (r, 1.0f);
                        }
                        if (k % 12 == 0 && ! black)
                        {
                            g.setColour (down || midi ? juce::Colour (0xff000000) : juce::Colour (0xff666666));
                            g.setFont (juce::FontOptions (11.0f));
                            g.drawText ("C" + juce::String (k / 12 - 1), r.withTrimmedBottom (4.0f).toNearestInt(), juce::Justification::centredBottom);
                        }
                    }
                g.restoreState();
                if (w.wheels != 0)
                {
                    const auto posIt = wheelPos_.find (w.id);
                    const std::array<float, 2> pos = posIt != wheelPos_.end() ? posIt->second : std::array<float, 2> { 0.0f, 0.0f };
                    for (int i = 0; i < 2; ++i)
                    {
                        const auto r = wheelRect (w, i);
                        const auto area = r.withTrimmedBottom (12.0f);
                        g.setColour (juce::Colour (0xff1a1a1a));
                        g.fillRoundedRectangle (area, 4.0f);
                        g.setColour (juce::Colour (0xff555555));
                        g.drawRoundedRectangle (area, 4.0f, 1.0f);
                        const float t = i == 0 ? (pos[0] + 1.0f) * 0.5f : pos[1];
                        const auto cap = juce::Rectangle<float> (area.getX() + 2.0f, area.getBottom() - 14.0f - t * (area.getHeight() - 14.0f), area.getWidth() - 4.0f, 14.0f);
                        g.setColour (o.colour (Token::Accent).withAlpha (mode_ == Mode::Wheel && wheelWidget_ == w.id && wheelIndex_ == i ? 1.0f : 0.75f));
                        g.fillRoundedRectangle (cap, 3.0f);
                        g.setColour (o.colour (Token::TextDim));
                        g.setFont (juce::FontOptions (9.0f));
                        g.drawText (i == 0 ? "PITCH" : "MOD", juce::Rectangle<float> (r).removeFromBottom (12.0f).toNearestInt(), juce::Justification::centred);
                    }
                }
                g.setColour (o.colour (Token::Border));
                g.drawRect (b, 1.0f);
                break;
            }
            case fc::WidgetType::Zone:
            {
                const auto r = b.reduced (1.0f);
                const auto* zstyle = w.style != 0 ? o.facade_.findStyle (w.style) : nullptr;
                const juce::Image frame = zstyle != nullptr ? picture (zstyle->frame) : juce::Image();
                if (frame.isValid())
                {
                    drawNineSlice (g, frame, naturalSize (zstyle->frame), *zstyle, b);
                    if (! w.text.empty())
                    {
                        const juce::Rectangle<float> titleArea (b.getX() + fc::zonePad, b.getY() + 4.0f, b.getWidth() - 2.0f * fc::zonePad, fc::zoneTitleHeight - 4.0f);
                        drawStyledText (g, zstyle, utf8 (w.text), titleArea, 0.75f, o.colour (Token::TextDim), juce::Justification::centredLeft);
                    }
                    break;
                }
                g.setColour (o.colour (Token::Pb).withAlpha (0.4f));
                g.fillRoundedRectangle (r, 10.0f);
                g.setColour (o.colour (Token::Border));
                g.drawRoundedRectangle (r, 10.0f, 1.5f);
                if (! w.text.empty())
                {
                    g.setColour (o.colour (Token::TextDim));
                    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
                    g.drawFittedText (utf8 (w.text), juce::Rectangle<int> (w.x + fc::zonePad, w.y + 4, w.w - 2 * fc::zonePad, fc::zoneTitleHeight - 4), juce::Justification::centredLeft, 1);
                }
                break;
            }
            case fc::WidgetType::Label:
                g.setColour (o.colour (Token::Text));
                g.setFont (juce::FontOptions (juce::jlimit (11.0f, 48.0f, b.getHeight() * 0.62f)));
                g.drawFittedText (utf8 (w.text), b.toNearestInt(), juce::Justification::centredLeft, 1);
                break;
        }
    }

    void paintEditOverlay (juce::Graphics& g, const fc::Widget& w) const
    {
        auto& o = owner_;
        const bool sel = o.selection_.count (w.id) != 0;
        const juce::Rectangle<float> b (static_cast<float> (w.x), static_cast<float> (w.y), static_cast<float> (w.w), static_cast<float> (w.h));
        g.setColour (sel ? o.colour (Token::Accent) : o.colour (Token::TextDim2).withAlpha (0.8f));
        g.drawRect (b, sel ? 2.0f : 1.0f);
        if (sel && o.selection_.size() == 1) g.fillRect (juce::Rectangle<float> (b.getRight() - 12.0f, b.getBottom() - 12.0f, 12.0f, 12.0f));
    }

    FacadeView& owner_;
    Mode mode_ = Mode::None;
    fc::Facade before_;
    juce::Point<float> dragStart_;
    juce::Rectangle<int> startRect_;
    juce::Rectangle<float> band_;
    std::set<int> bandBase_;
    std::map<int, std::pair<int, int>> pressed_;   // doigt ou souris -> (clavier, touche) tenu
    std::map<int, std::array<float, 2>> wheelPos_;   // clavier -> position des molettes (hauteur -1..+1, modulation 0..1)
    std::map<int, std::array<int, 2>> wheelSent_;    // clavier -> dernière valeur MIDI envoyée par chaque molette
    int wheelWidget_ = 0, wheelIndex_ = 0;           // molette tenue par la souris
    std::map<int, std::set<int>> latched_;         // clavier -> touches en maintien
    std::set<int> moving_;   // widgets entraînés par un déplacement : la sélection et le contenu des zones sélectionnées
    neptune::core::ParamId playId_ = 0;
    double startNorm_ = 0.0;
    float startY_ = 0.0f;
    int slideId_ = 0, turnId_ = 0;
    struct Picture { juce::Image image; juce::Point<float> natural; };
    mutable std::map<std::string, Picture> pictures_;   // chemin -> image (vide : introuvable)
    std::map<std::string, std::string> svgVars_;        // {{accent}}... : couleurs du thème pour les SVG
    int hoverId_ = 0;
    struct MeterState { float level[2] {}, hold[2] {}; int age[2] {}; };   // dB affichés, crêtes maintenues, âge du maintien (images)
    std::map<int, MeterState> meters_;
    int midiId_ = 0;                          // widget « Message MIDI » en cours d'appui
    std::map<int, int> midiLast_;             // dernière valeur envoyée par chaque curseur MIDI
    std::map<int, double> midiFader_;         // position affichée de chaque curseur MIDI (0..1)
    int pistonId_ = 0;           // piston en cours d'appui (rappel au relâchement, mémorisation si l'appui est long)
    double bankHoldStartMs_ = 0.0;
    int bankDir_ = 0, bankHoldId_ = 0;   // grille de scènes : flèche de banque tenue (-1 précédente, +1 suivante) et son widget
    struct BankHold : juce::Timer
    {
        std::function<void()> fire;
        void timerCallback() override { if (fire) fire(); }
    } bankHold_;
    bool longPress_ = false;
    std::map<int, std::unique_ptr<VelocityPanel>> panels_;
    std::uint32_t panelRevision_ = 0xFFFFFFFFu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Canvas)
};

// ============================== Choix du ou des paramètres ==============================

// Un paramètre (liaison d'un widget) ou plusieurs (instantané d'un bouton d'action : coche par clic).
class FacadeView::Picker : public juce::Component, private juce::ListBoxModel
{
public:
    Picker (FacadeView& owner, int track, bool capture, std::set<neptune::core::ParamId> initial,
            std::function<void (neptune::core::ParamId)> onChoose, std::function<void (const std::set<neptune::core::ParamId>&)> onCapture)
        : owner_ (owner), capture_ (capture), chosen_ (std::move (initial)), onChoose_ (std::move (onChoose)), onCapture_ (std::move (onCapture))
    {
        auto& ps = owner.processor_.parameters();
        for (std::size_t i = 0; i < ps.size(); ++i)
        {
            const auto& s = ps.spec (i);
            if (! s.hidden) entries_.push_back ({ s.id, utf8 (s.name), neptune::core::paramgroups::classify (s.name) });
        }
        // Les Groupes du Sampler de chaque piste (les jeux d'un orgue) : un bouton lié à l'un d'eux le met en jeu ou le coupe, sans passer par le
        // réglage des modulateurs ; l'interrupteur est créé au choix (ensureGroupSwitch).
        if (! capture)
            for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
            {
                if (! owner.processor_.trackActive (t) || owner.processor_.trackType (t) != neptune::core::TrackType::Sampler) continue;
                const auto scope = neptune::core::paramgroups::classify ("Track " + std::to_string (t + 1) + " Modulator 1");
                for (const auto& name : neptune::core::mapping::groupNames (owner.processor_.instrumentDescription (t)))
                {
                    const auto id = kGroupBase + static_cast<neptune::core::ParamId> (groupEntries_.size());
                    groupEntries_.push_back ({ id, t, name });
                    entries_.push_back ({ id, utf8 (owner.tr_.format ("facade.group_entry", { std::to_string (t + 1), name })), scope });
                }
            }
        title_.setText (owner.text (capture ? "facade.capture_title" : "facade.picker_title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        hint_.setText (owner.text (capture ? "facade.capture_hint" : "facade.picker_hint"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
        addAndMakeVisible (title_);
        addAndMakeVisible (hint_);
        {
            std::vector<neptune::core::paramgroups::Group> groups;
            for (const auto& e : entries_) groups.push_back (e.group);
            bar_ = std::make_unique<ParamFilterBar> (groups, owner.palette_, owner.tr_, track, [this] { refilter(); });
            addAndMakeVisible (*bar_);
        }
        filter_.setFont (juce::FontOptions (18.0f));
        filter_.onTextChange = [this] { refilter(); };
        filter_.onReturnKey = [this] { if (capture_) confirm(); else choose (list_.getSelectedRow()); };
        filter_.onEscapeKey = [this] { close(); };
        addAndMakeVisible (filter_);
        list_.setModel (this);
        list_.setRowHeight (26);
        list_.setColour (juce::ListBox::backgroundColourId, owner.colour (Token::BgApp));
        addAndMakeVisible (list_);
        cancel_.setButtonText (owner.text ("facade.picker_cancel"));
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible (cancel_);
        if (capture)
        {
            allButton_.setButtonText (owner.text ("facade.capture_all"));
            allButton_.setTooltip (owner.text ("facade.tip_capture_all"));
            allButton_.onClick = [this, track]
            {
                for (const auto id : neptune::core::facade::instrumentParamsOfTrack (owner_.processor_.parameters(), track)) chosen_.insert (id);
                checkedOnly_.setToggleState (true, juce::dontSendNotification);   // la liste montre alors ce qui est coché : le geste est visible
                refilter();
                updateCount();
            };
            addAndMakeVisible (allButton_);
            allTracksButton_.setButtonText (owner.text ("facade.capture_all_tracks"));
            allTracksButton_.setTooltip (owner.text ("facade.tip_capture_all_tracks"));
            allTracksButton_.onClick = [this]
            {
                // toutes les pistes actives (un orgue à plusieurs claviers) et les accouplements du routeur
                for (int t = 0; t < neptune::core::mixer::numTracks; ++t)
                    if (owner_.processor_.trackActive (t))
                        for (const auto id : neptune::core::facade::instrumentParamsOfTrack (owner_.processor_.parameters(), t)) chosen_.insert (id);
                for (int i = 0; i < neptune::core::router::kSwitches; ++i) chosen_.insert (neptune::core::router::switchParam (i));
                for (int i = 0; i < neptune::core::router::kMaxRules; ++i) chosen_.insert (neptune::core::router::transposeParam (i));
                checkedOnly_.setToggleState (true, juce::dontSendNotification);
                refilter();
                updateCount();
            };
            addAndMakeVisible (allTracksButton_);
            checkedOnly_.setButtonText (owner.text ("facade.capture_checked_only"));
            checkedOnly_.setColour (juce::ToggleButton::textColourId, owner.colour (Token::Text));
            checkedOnly_.onClick = [this] { refilter(); };
            addAndMakeVisible (checkedOnly_);
            count_.setColour (juce::Label::textColourId, owner.colour (Token::Accent));
            addAndMakeVisible (count_);
            ok_.setButtonText (owner.text ("facade.capture_ok"));
            ok_.onClick = [this] { confirm(); };
            addAndMakeVisible (ok_);
        }
        else
        {
            unlink_.setButtonText (owner.text ("facade.picker_unlink"));
            unlink_.onClick = [this] { if (onChoose_) onChoose_ (0); close(); };
            addAndMakeVisible (unlink_);
        }
        refilter();
        updateCount();
        setWantsKeyboardFocus (true);
    }

    void visibilityChanged() override { if (isVisible()) filter_.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (16);
        title_.setBounds (r.removeFromTop (30));
        hint_.setBounds (r.removeFromTop (36));
        filter_.setBounds (r.removeFromTop (32));
        r.removeFromTop (6);
        bar_->setBounds (r.removeFromTop (bar_->preferredHeight (r.getWidth())));
        r.removeFromTop (6);
        if (capture_)
        {
            auto info = r.removeFromTop (26);
            count_.setBounds (info.removeFromLeft (250));
            checkedOnly_.setBounds (info);
            r.removeFromTop (4);
        }
        auto bottom = r.removeFromBottom (32);
        r.removeFromBottom (8);
        list_.setBounds (r);
        cancel_.setBounds (bottom.removeFromRight (120));
        bottom.removeFromRight (8);
        (capture_ ? ok_ : unlink_).setBounds (bottom.removeFromRight (140));
        if (capture_) { allButton_.setBounds (bottom.removeFromLeft (260)); bottom.removeFromLeft (6); allTracksButton_.setBounds (bottom.removeFromLeft (260)); }
    }

private:
    struct Entry { neptune::core::ParamId id; juce::String name; neptune::core::paramgroups::Group group; };
    struct GroupEntry { neptune::core::ParamId id; int track; std::string name; };
    static constexpr neptune::core::ParamId kGroupBase = 0x80000000u;   // identifiants fictifs des Groupes dans la liste (aucun vrai paramètre n'y arrive)

    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (560, juce::jmin (700, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void updateCount()
    {
        if (capture_) count_.setText (juce::String::fromUTF8 (owner_.tr_.format ("facade.capture_count", { std::to_string (chosen_.size()) }).c_str()), juce::dontSendNotification);
    }

    void refilter()
    {
        shown_.clear();
        if (capture_ && checkedOnly_.getToggleState())
        {
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (chosen_.count (entries_[i].id) != 0) shown_.push_back (i);
            list_.updateContent();
            list_.selectRow (-1);
            list_.repaint();
            return;
        }
        const auto words = juce::StringArray::fromTokens (filter_.getText().trim(), " ", "");
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            bool ok = bar_ == nullptr || bar_->accepts (entries_[i].group);
            for (const auto& w : words)
                if (w.isNotEmpty() && ! entries_[i].name.containsIgnoreCase (w)) { ok = false; break; }
            if (ok) shown_.push_back (i);
        }
        list_.updateContent();
        list_.selectRow (shown_.empty() ? -1 : 0);
        list_.repaint();
    }

    void choose (int row)
    {
        if (row < 0 || row >= static_cast<int> (shown_.size())) return;
        auto id = entries_[shown_[static_cast<std::size_t> (row)]].id;
        if (id >= kGroupBase)
        {
            // un Groupe : on le relie à son propre modulateur (créé au besoin), et le bouton se lie à ce modulateur
            const auto it = std::find_if (groupEntries_.begin(), groupEntries_.end(), [id] (const GroupEntry& g) { return g.id == id; });
            id = it != groupEntries_.end() ? owner_.processor_.ensureGroupSwitch (it->track, it->name) : 0;
            if (id == 0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, owner_.text ("facade.group_switch_title"), owner_.text ("facade.group_switch_full"));
                return;
            }
        }
        if (onChoose_) onChoose_ (id);
        close();
    }

    void confirm()
    {
        if (onCapture_) onCapture_ (chosen_);
        close();
    }

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->picker_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    int getNumRows() override { return static_cast<int> (shown_.size()); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= static_cast<int> (shown_.size())) return;
        const auto& e = entries_[shown_[static_cast<std::size_t> (row)]];
        if (selected) { g.setColour (owner_.colour (Token::Accent).withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
        int x = 8;
        if (capture_)
        {
            const juce::Rectangle<float> box (8.0f, h * 0.5f - 7.0f, 14.0f, 14.0f);
            g.setColour (owner_.colour (Token::Border));
            g.drawRect (box, 1.5f);
            if (chosen_.count (e.id) != 0) { g.setColour (owner_.colour (Token::Accent)); g.fillRect (box.reduced (3.0f)); }
            x = 32;
        }
        g.setColour (owner_.colour (Token::Text));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (e.name, x, 0, w - x - 8, h, juce::Justification::centredLeft);
    }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (! capture_ || row < 0 || row >= static_cast<int> (shown_.size())) return;
        const auto id = entries_[shown_[static_cast<std::size_t> (row)]].id;
        if (chosen_.count (id) != 0) chosen_.erase (id); else chosen_.insert (id);
        updateCount();
        if (checkedOnly_.getToggleState()) refilter(); else list_.repaint();
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (! capture_) choose (row); }

    FacadeView& owner_;
    bool capture_;
    std::set<neptune::core::ParamId> chosen_;
    std::function<void (neptune::core::ParamId)> onChoose_;
    std::function<void (const std::set<neptune::core::ParamId>&)> onCapture_;
    std::vector<Entry> entries_;
    std::vector<GroupEntry> groupEntries_;
    std::vector<std::size_t> shown_;
    juce::Label title_, hint_;
    juce::TextEditor filter_;
    std::unique_ptr<ParamFilterBar> bar_;
    juce::ListBox list_;
    juce::TextButton cancel_, unlink_, ok_, allButton_, allTracksButton_;
    juce::ToggleButton checkedOnly_;
    juce::Label count_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Picker)
};

// ============================== Saisie de texte et aide (calques de la vue) ==============================

// Saisie d'un texte dans un calque de la vue (pas de fenêtre système : fiable dans un hôte), avec OK et Annuler bien visibles.
class FacadeView::TextPrompt : public juce::Component
{
public:
    TextPrompt (FacadeView& owner, const juce::String& title, const juce::String& initial, std::function<void (const juce::String&)> onDone)
        : owner_ (owner), onDone_ (std::move (onDone))
    {
        title_.setText (title, juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        editor_.setText (initial, false);
        editor_.setFont (juce::FontOptions (20.0f));
        editor_.setSelectAllWhenFocused (true);
        editor_.setInputRestrictions (static_cast<int> (fc::maxTextLength));
        editor_.onReturnKey = [this] { accept(); };
        editor_.onEscapeKey = [this] { close(); };
        addAndMakeVisible (editor_);
        ok_.setButtonText (owner.text ("facade.ok"));
        ok_.onClick = [this] { accept(); };
        cancel_.setButtonText (owner.text ("facade.picker_cancel"));
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible (ok_);
        addAndMakeVisible (cancel_);
    }

    void visibilityChanged() override { if (isVisible()) editor_.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        r.removeFromTop (8);
        editor_.setBounds (r.removeFromTop (44));
        r.removeFromTop (16);
        auto row = r.removeFromTop (44);
        cancel_.setBounds (row.removeFromRight (150));
        row.removeFromRight (10);
        ok_.setBounds (row.removeFromRight (150));
    }

private:
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (480, 190).withCentre (getLocalBounds().getCentre()); }

    void accept()
    {
        const auto text = editor_.getText();
        auto done = onDone_;
        close();
        if (done) done (text);
    }

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->prompt_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    FacadeView& owner_;
    std::function<void (const juce::String&)> onDone_;
    juce::Label title_;
    juce::TextEditor editor_;
    juce::TextButton ok_, cancel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TextPrompt)
};

// Aide : ce que fait chaque élément de la Façade.
class FacadeView::HelpPanel : public juce::Component
{
public:
    HelpPanel (FacadeView& owner, const juce::String& title, const juce::String& body) : owner_ (owner)
    {
        title_.setText (title, juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        body_.setMultiLine (true, true);
        body_.setReadOnly (true);
        body_.setCaretVisible (false);
        body_.setFont (juce::FontOptions (16.0f));
        body_.setColour (juce::TextEditor::backgroundColourId, owner.colour (Token::BgApp));
        body_.setColour (juce::TextEditor::textColourId, owner.colour (Token::Text));
        body_.setColour (juce::TextEditor::outlineColourId, owner.colour (Token::Border));
        body_.setText (body, false);
        addAndMakeVisible (body_);
        close_.setButtonText (owner.text ("modal.close"));
        close_.onClick = [this] { close(); };
        addAndMakeVisible (close_);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (44);
        r.removeFromBottom (10);
        body_.setBounds (r);
        close_.setBounds (bottom.removeFromRight (150));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { close(); return true; }
        return false;
    }

private:
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (720, juce::jmin (640, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->help_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    FacadeView& owner_;
    juce::Label title_;
    juce::TextEditor body_;
    juce::TextButton close_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpPanel)
};

// ============================== Bibliothèque de styles d'images ==============================

// Fenêtre des styles : liste, nom, une image par usage (bouton éteint / allumé, rail et poignée de curseur, bande de potard).
class FacadeView::StylePanel : public juce::Component, private juce::ListBoxModel
{
    friend class FacadeView;   // l'installation par glisser-déposer passe par setMessage et owner_message
public:
    explicit StylePanel (FacadeView& owner) : owner_ (owner)
    {
        title_.setText (owner.text ("facade.styles_title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        list_.setModel (this);
        list_.setRowHeight (28);
        list_.setColour (juce::ListBox::backgroundColourId, owner.colour (Token::BgApp));
        addAndMakeVisible (list_);
        newButton_.setButtonText (owner.text ("facade.style_new"));
        newButton_.onClick = [this] { createStyle(); };
        deleteButton_.setButtonText (owner.text ("facade.delete"));
        deleteButton_.onClick = [this] { deleteStyle(); };
        closeButton_.setButtonText (owner.text ("modal.close"));
        closeButton_.onClick = [this] { close(); };
        examplesButton_.setButtonText (owner.text ("facade.examples"));
        examplesButton_.setTooltip (owner.text ("facade.tip_examples"));
        examplesButton_.onClick = [this] { createExamples(); };
        libraryButton_.setButtonText (owner.text ("facade.library"));
        libraryButton_.setTooltip (owner.text ("facade.tip_library"));
        libraryButton_.onClick = [this] { showLibraryMenu(); };
        for (auto* b : { &newButton_, &deleteButton_, &closeButton_, &examplesButton_, &libraryButton_ }) addAndMakeVisible (*b);

        nameLabel_.setText (owner.text ("facade.style_name"), juce::dontSendNotification);
        nameLabel_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (nameLabel_);
        name_.setFont (juce::FontOptions (18.0f));
        name_.setInputRestrictions (static_cast<int> (fc::maxTextLength));
        name_.onReturnKey = [this] { commitName(); };
        name_.onFocusLost = [this] { commitName(); };
        addAndMakeVisible (name_);

        addAndMakeVisible (view_);
        view_.setViewedComponent (&content_, false);
        view_.setScrollBarsShown (true, false);
        buildRows();
        hint_.setText (owner.text ("facade.style_hint"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
        addAndMakeVisible (hint_);
        addAndMakeVisible (preview_);

        list_.updateContent();
        if (! owner_.facade_.styles.empty()) list_.selectRow (0);
        syncFields();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (40);
        closeButton_.setBounds (bottom.removeFromRight (140));
        libraryButton_.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (8);
        examplesButton_.setBounds (bottom.removeFromLeft (200));
        bottom.removeFromLeft (10);
        hint_.setBounds (bottom);
        r.removeFromBottom (8);
        auto left = r.removeFromLeft (220);
        auto leftButtons = left.removeFromBottom (40);
        left.removeFromBottom (6);
        list_.setBounds (left);
        newButton_.setBounds (leftButtons.removeFromLeft (leftButtons.getWidth() / 2).reduced (2, 0));
        deleteButton_.setBounds (leftButtons.reduced (2, 0));
        r.removeFromLeft (14);
        auto nameRow = r.removeFromTop (34);
        nameLabel_.setBounds (nameRow.removeFromLeft (110));
        name_.setBounds (nameRow);
        r.removeFromTop (10);
        auto previewArea = r.removeFromBottom (juce::jmin (170, r.getHeight() / 2));
        r.removeFromBottom (8);
        view_.setBounds (r);
        layoutContent();
        preview_.setBounds (previewArea);

    }

private:
    // Aperçu du style sélectionné : un bouton à cliquer (éteint / allumé), un potard et deux curseurs à glisser, pour voir tous les états.
    class Preview : public juce::Component
    {
    public:
        explicit Preview (StylePanel& panel) : panel_ (panel) {}

        void resized() override
        {
            auto r = getLocalBounds().toFloat().withTrimmedTop (18.0f).reduced (4.0f);
            const float cw = r.getWidth() / 4.0f;
            for (auto& c : cells_) { c = r.removeFromLeft (cw).reduced (6.0f); }
        }

        void paint (juce::Graphics& g) override
        {
            auto& o = panel_.owner_;
            g.setColour (o.colour (Token::BgApp));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
            g.setColour (o.colour (Token::TextDim));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (o.text ("facade.style_preview"), getLocalBounds().removeFromTop (18).reduced (8, 0), juce::Justification::centredLeft);
            const auto* st = panel_.current();
            if (st == nullptr) return;
            auto pic = [&o] (const std::string& path, int side = 1024) { return o.canvas_->cachedPicture (path, side); };

            // 0 : bouton
            {
                const auto face = pic (on_ ? st->on : st->off);
                if (face.isValid()) g.drawImage (face, cells_[0], juce::RectanglePlacement::centred);
                else
                {
                    const auto r = cells_[0].withSizeKeepingCentre (cells_[0].getWidth(), 34.0f);
                    g.setColour (on_ ? o.colour (Token::Accent) : o.colour (Token::Pb));
                    g.fillRoundedRectangle (r, 8.0f);
                    g.setColour (o.colour (Token::Border));
                    g.drawRoundedRectangle (r, 8.0f, 1.5f);
                }
            }
            // 1 : potard
            {
                const auto pointer = pic (st->pointer);
                const auto strip = st->frames > 1 ? pic (st->strip, 4096) : juce::Image();
                if (pointer.isValid())
                {
                    const auto body = pic (st->body);
                    const auto& ref = body.isValid() ? body : pointer;
                    const auto dest = juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (ref.getWidth()), static_cast<float> (ref.getHeight())), cells_[1]);
                    if (body.isValid()) g.drawImage (body, dest, juce::RectanglePlacement::stretchToFit);
                    const float degrees = static_cast<float> (st->angleMin) + static_cast<float> (knob_) * static_cast<float> (st->angleMax - st->angleMin);
                    juce::Graphics::ScopedSaveState keep (g);
                    g.addTransform (juce::AffineTransform::rotation (juce::degreesToRadians (degrees), dest.getX() + dest.getWidth() * static_cast<float> (st->pivotX) / 100.0f,
                                                                      dest.getY() + dest.getHeight() * static_cast<float> (st->pivotY) / 100.0f));
                    g.drawImage (pointer, dest, juce::RectanglePlacement::stretchToFit);
                }
                else if (strip.isValid())
                {
                    const int frames = st->frames, frame = juce::jlimit (0, frames - 1, juce::roundToInt (static_cast<float> (knob_) * static_cast<float> (frames - 1)));
                    const int fw = st->stripHorizontal ? strip.getWidth() / frames : strip.getWidth();
                    const int fh = st->stripHorizontal ? strip.getHeight() : strip.getHeight() / frames;
                    if (fw > 0 && fh > 0)
                    {
                        const auto dest = juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (fw), static_cast<float> (fh)), cells_[1]).toNearestInt();
                        g.drawImage (strip, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(),
                                     st->stripHorizontal ? frame * fw : 0, st->stripHorizontal ? 0 : frame * fh, fw, fh);
                    }
                }
                else paintNeptuneKnob (g, cells_[1], static_cast<float> (knob_), o.palette_, o.knobStyle_);
            }
            // 2 et 3 : curseurs horizontal et vertical
            for (int i = 0; i < 2; ++i)
            {
                const bool vertical = i == 1;
                auto area = cells_[static_cast<std::size_t> (2 + i)];
                if (! vertical) area = area.withSizeKeepingCentre (area.getWidth(), std::min (area.getHeight(), 40.0f));
                else area = area.withSizeKeepingCentre (std::min (area.getWidth(), 40.0f), area.getHeight());
                const auto rail = pic (st->track), handle = pic (st->handle);
                if (rail.isValid()) g.drawImage (rail, area, juce::RectanglePlacement::stretchToFit);
                else
                {
                    g.setColour (o.colour (Token::Border));
                    g.fillRoundedRectangle (vertical ? juce::Rectangle<float> (4.0f, area.getHeight()).withCentre (area.getCentre()) : juce::Rectangle<float> (area.getWidth(), 4.0f).withCentre (area.getCentre()), 2.0f);
                }
                fc::Widget fake;
                fake.vertical = vertical ? 1 : 0;
                fake.w = juce::roundToInt (area.getWidth());
                fake.h = juce::roundToInt (area.getHeight());
                const float len = sliderHandleLength (fake, handle);
                const float travel = (vertical ? area.getHeight() : area.getWidth()) - len;
                const float pos = static_cast<float> (i == 0 ? sliderH_ : sliderV_);
                const auto hr = vertical ? juce::Rectangle<float> (area.getX(), area.getBottom() - len - pos * travel, area.getWidth(), len)
                                         : juce::Rectangle<float> (area.getX() + pos * travel, area.getY(), len, area.getHeight());
                if (handle.isValid()) g.drawImage (handle, hr, juce::RectanglePlacement::centred);
                else
                {
                    g.setColour (o.colour (Token::Accent));
                    g.fillRoundedRectangle (hr.reduced (1.0f), 4.0f);
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            active_ = -1;
            for (int i = 0; i < 4; ++i)
                if (cells_[static_cast<std::size_t> (i)].contains (e.position)) active_ = i;
            if (active_ == 0) { on_ = ! on_; repaint(); }
            startKnob_ = knob_;
            update (e);
        }
        void mouseDrag (const juce::MouseEvent& e) override { update (e); }
        void mouseUp (const juce::MouseEvent&) override { active_ = -1; }

    private:
        void update (const juce::MouseEvent& e)
        {
            if (active_ == 1) knob_ = juce::jlimit (0.0, 1.0, startKnob_ + (e.getMouseDownPosition().y - e.position.y) / 150.0);
            else if (active_ == 2)
            {
                const auto& c = cells_[2];
                sliderH_ = juce::jlimit (0.0, 1.0, static_cast<double> ((e.position.x - c.getX()) / std::max (1.0f, c.getWidth())));
            }
            else if (active_ == 3)
            {
                const auto& c = cells_[3];
                sliderV_ = juce::jlimit (0.0, 1.0, 1.0 - static_cast<double> ((e.position.y - c.getY()) / std::max (1.0f, c.getHeight())));
            }
            repaint();
        }

        StylePanel& panel_;
        std::array<juce::Rectangle<float>, 4> cells_;
        bool on_ = false;
        double knob_ = 0.5, sliderH_ = 0.5, sliderV_ = 0.5, startKnob_ = 0.5;
        int active_ = -1;
    };

    struct ImageRow
    {
        std::string fc::ImageStyle::* member = nullptr;
        juce::Label label, path;
        juce::TextButton choose, clear;
    };
    struct IntRow
    {
        int fc::ImageStyle::* member = nullptr;
        juce::Label label;
        juce::TextEditor field;
    };
    struct TextRow
    {
        std::string fc::ImageStyle::* member = nullptr;
        juce::Label label;
        juce::TextEditor field;
    };

    // Construit les lignes de réglage : une image par rôle, puis les options (nombre d'images, angles, découpe, texte...).
    void buildRows()
    {
        auto caption = [this] (juce::Label& l, const char* key)
        {
            l.setText (owner_.text (key), juce::dontSendNotification);
            l.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            l.setColour (juce::Label::textColourId, owner_.colour (Token::Accent));
            content_.addAndMakeVisible (l);
        };
        caption (imagesCaption_, "facade.style_images");
        const auto& roles = fc::imageRoles();
        for (std::size_t i = 0; i < roles.size(); ++i)
        {
            auto row = std::make_unique<ImageRow>();
            row->member = roles[i].field;
            row->label.setText (owner_.text (("facade.role_" + std::string (roles[i].name)).c_str()), juce::dontSendNotification);
            row->label.setColour (juce::Label::textColourId, owner_.colour (Token::Text));
            row->path.setColour (juce::Label::textColourId, owner_.colour (Token::TextDim));
            row->path.setMinimumHorizontalScale (0.7f);
            row->choose.setButtonText (owner_.text ("facade.style_choose"));
            row->choose.onClick = [this, i] { chooseImage (i); };
            row->clear.setButtonText (owner_.text ("facade.style_clear"));
            row->clear.onClick = [this, i] { const auto field = fc::imageRoles()[i].field; edit ([field] (fc::ImageStyle& st) { st.*field = std::string(); }); };
            for (auto* c : std::initializer_list<juce::Component*> { &row->label, &row->path, &row->choose, &row->clear }) content_.addAndMakeVisible (*c);
            imageRows_.push_back (std::move (row));
        }

        caption (optionsCaption_, "facade.style_options");
        struct IntOption { const char* key; int fc::ImageStyle::* member; };
        static const IntOption ints[] = { { "facade.opt_frames", &fc::ImageStyle::frames }, { "facade.opt_angle_min", &fc::ImageStyle::angleMin }, { "facade.opt_angle_max", &fc::ImageStyle::angleMax },
                                          { "facade.opt_pivot_x", &fc::ImageStyle::pivotX }, { "facade.opt_pivot_y", &fc::ImageStyle::pivotY },
                                          { "facade.opt_slice_left", &fc::ImageStyle::sliceLeft }, { "facade.opt_slice_top", &fc::ImageStyle::sliceTop },
                                          { "facade.opt_slice_right", &fc::ImageStyle::sliceRight }, { "facade.opt_slice_bottom", &fc::ImageStyle::sliceBottom },
                                          { "facade.opt_slice_scale", &fc::ImageStyle::sliceScale }, { "facade.opt_text_size", &fc::ImageStyle::textSize } };
        for (const auto& opt : ints)
        {
            auto row = std::make_unique<IntRow>();
            row->member = opt.member;
            row->label.setText (owner_.text (opt.key), juce::dontSendNotification);
            row->label.setColour (juce::Label::textColourId, owner_.colour (Token::Text));
            row->field.setInputRestrictions (5, "-0123456789");
            row->field.setFont (juce::FontOptions (17.0f));
            auto* field = row.get();
            row->field.onReturnKey = [this, field] { commitInt (*field); };
            row->field.onFocusLost = [this, field] { commitInt (*field); };
            content_.addAndMakeVisible (row->label);
            content_.addAndMakeVisible (row->field);
            intRows_.push_back (std::move (row));
        }
        struct TextOption { const char* key; std::string fc::ImageStyle::* member; };
        static const TextOption texts[] = { { "facade.opt_text_colour", &fc::ImageStyle::textColour }, { "facade.opt_key_lit", &fc::ImageStyle::keyLit } };
        for (const auto& opt : texts)
        {
            auto row = std::make_unique<TextRow>();
            row->member = opt.member;
            row->label.setText (owner_.text (opt.key), juce::dontSendNotification);
            row->label.setColour (juce::Label::textColourId, owner_.colour (Token::Text));
            row->field.setInputRestrictions (9);
            row->field.setFont (juce::FontOptions (17.0f));
            auto* field = row.get();
            row->field.onReturnKey = [this, field] { commitText (*field); };
            row->field.onFocusLost = [this, field] { commitText (*field); };
            content_.addAndMakeVisible (row->label);
            content_.addAndMakeVisible (row->field);
            textRows_.push_back (std::move (row));
        }
        positionLabel_.setText (owner_.text ("facade.opt_text_position"), juce::dontSendNotification);
        positionLabel_.setColour (juce::Label::textColourId, owner_.colour (Token::Text));
        content_.addAndMakeVisible (positionLabel_);
        const char* places[5] = { "facade.pos_centre", "facade.pos_top", "facade.pos_bottom", "facade.pos_left", "facade.pos_right" };
        for (int i = 0; i < 5; ++i) position_.addItem (owner_.text (places[i]), i + 1);
        position_.onChange = [this] { if (! syncing_) edit ([this] (fc::ImageStyle& st) { st.textPosition = position_.getSelectedId() - 1; }); };
        content_.addAndMakeVisible (position_);
        for (auto* t : { &bold_, &stripH_ })
        {
            t->setColour (juce::ToggleButton::textColourId, owner_.colour (Token::Text));
            content_.addAndMakeVisible (*t);
        }
        bold_.setButtonText (owner_.text ("facade.opt_text_bold"));
        bold_.onClick = [this] { if (! syncing_) edit ([this] (fc::ImageStyle& st) { st.textBold = bold_.getToggleState(); }); };
        stripH_.setButtonText (owner_.text ("facade.style_strip_h"));
        stripH_.onClick = [this] { if (! syncing_) edit ([this] (fc::ImageStyle& st) { st.stripHorizontal = stripH_.getToggleState(); }); };
    }

    void layoutContent()
    {
        const int width = std::max (300, view_.getMaximumVisibleWidth());
        int y = 0;
        auto line = [&y, width] (int h = 34) { const juce::Rectangle<int> r (0, y, width - 6, h); y += h + 4; return r; };
        imagesCaption_.setBounds (line (26));
        for (auto& row : imageRows_)
        {
            auto l = line();
            row->label.setBounds (l.removeFromLeft (190));
            row->clear.setBounds (l.removeFromRight (80).reduced (2, 0));
            row->choose.setBounds (l.removeFromRight (110).reduced (2, 0));
            row->path.setBounds (l);
        }
        y += 6;
        optionsCaption_.setBounds (line (26));
        for (auto& row : intRows_)
        {
            auto l = line();
            row->label.setBounds (l.removeFromLeft (260));
            row->field.setBounds (l.removeFromLeft (90));
        }
        stripH_.setBounds (line());
        for (auto& row : textRows_)
        {
            auto l = line();
            row->label.setBounds (l.removeFromLeft (260));
            row->field.setBounds (l.removeFromLeft (130));
        }
        auto p = line();
        positionLabel_.setBounds (p.removeFromLeft (260));
        position_.setBounds (p.removeFromLeft (180));
        bold_.setBounds (line());
        contentHeight_ = y;
        content_.setSize (width, contentHeight_);
    }

    void commitInt (IntRow& row)
    {
        const int value = row.field.getText().getIntValue();
        const auto member = row.member;
        if (const auto* st = current(); st != nullptr && st->*member != value) edit ([member, value] (fc::ImageStyle& s) { s.*member = value; });
        else syncFields();
    }

    void commitText (TextRow& row)
    {
        const std::string value = row.field.getText().toStdString();
        const auto member = row.member;
        if (const auto* st = current(); st != nullptr && st->*member != value) edit ([member, value] (fc::ImageStyle& s) { s.*member = value; });
    }

    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (800, juce::jmin (660, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    const fc::ImageStyle* current() const
    {
        const int row = list_.getSelectedRow();
        const auto& styles = owner_.facade_.styles;
        return row >= 0 && row < static_cast<int> (styles.size()) ? &styles[static_cast<std::size_t> (row)] : nullptr;
    }

    // Change le style sélectionné (un seul geste annulable) puis relit les champs.
    void edit (const std::function<void (fc::ImageStyle&)>& change)
    {
        const int row = list_.getSelectedRow();
        auto f = owner_.facade_;
        if (row < 0 || row >= static_cast<int> (f.styles.size())) return;
        change (f.styles[static_cast<std::size_t> (row)]);
        owner_.commit (std::move (f), "Facade: styles");
        owner_.canvas_->clearPictures();
        list_.updateContent();
        list_.repaint();
        syncFields();
    }

    void syncFields()
    {
        const auto* st = current();
        const bool has = st != nullptr;
        syncing_ = true;
        name_.setEnabled (has);
        deleteButton_.setEnabled (has);
        name_.setText (has ? utf8 (st->name) : juce::String(), false);
        for (auto& r : imageRows_)
        {
            r->choose.setEnabled (has);
            r->clear.setEnabled (has);
            const std::string path = has ? st->*(r->member) : std::string();
            const bool missing = ! path.empty() && ! stylelib::exists (path);
            r->path.setText (path.empty() ? owner_.text ("facade.style_none") : juce::File (utf8 (path)).getFileName() + (missing ? "  (" + owner_.text ("facade.missing") + ")" : juce::String()), juce::dontSendNotification);
            r->path.setColour (juce::Label::textColourId, owner_.colour (missing ? Token::KnobHot : Token::TextDim));
        }
        for (auto& r : intRows_) { r->field.setEnabled (has); r->field.setText (has ? juce::String (st->*(r->member)) : juce::String(), false); }
        for (auto& r : textRows_) { r->field.setEnabled (has); r->field.setText (has ? utf8 (st->*(r->member)) : juce::String(), false); }
        position_.setEnabled (has);
        position_.setSelectedId (has ? st->textPosition + 1 : 1, juce::dontSendNotification);
        bold_.setEnabled (has);
        bold_.setToggleState (has && st->textBold, juce::dontSendNotification);
        stripH_.setEnabled (has);
        stripH_.setToggleState (has && st->stripHorizontal, juce::dontSendNotification);
        syncing_ = false;
        preview_.repaint();
    }

    void commitName()
    {
        const auto value = name_.getText().toStdString();
        if (const auto* st = current(); st != nullptr && st->name != value) edit ([value] (fc::ImageStyle& s) { s.name = value; });
    }

    void chooseImage (std::size_t index)
    {
        chooser_ = std::make_unique<juce::FileChooser> (owner_.text ("facade.choose_image"), juce::File::getSpecialLocation (juce::File::userPicturesDirectory), "*.png;*.jpg;*.jpeg;*.svg");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [safe = juce::Component::SafePointer<StylePanel> (this), index] (const juce::FileChooser& c)
                               {
                                   if (safe == nullptr) return;
                                   const auto file = c.getResult();
                                   if (! file.existsAsFile()) return;
                                   const auto member = fc::imageRoles()[index].field;
                                   const std::string path = file.getFullPathName().toStdString();
                                   safe->edit ([member, path] (fc::ImageStyle& st) { st.*member = path; });
                               });
    }

    void setMessage (const juce::String& message) { hint_.setText (message, juce::dontSendNotification); }

    // Ajoute à la Façade un style d'un pack de la bibliothèque (ses images restent dans le pack, désignées par des références).
    int addPackStyles (const stylelib::Pack& pack, int onlyIndex = -1)
    {
        auto f = owner_.facade_;
        int added = 0, selectRow = -1;
        for (int i = 0; i < static_cast<int> (pack.info.styles.size()); ++i)
        {
            if (onlyIndex >= 0 && i != onlyIndex) continue;
            const auto& ps = pack.info.styles[static_cast<std::size_t> (i)];
            fc::ImageStyle st = ps.style;
            st.id = 0;
            for (const auto& role : fc::imageRoles())
                if (! (st.*(role.field)).empty()) st.*(role.field) = neptune::core::stylepack::makeReference (pack.info.id, st.*(role.field));
            const auto known = std::find_if (f.styles.begin(), f.styles.end(), [&st] (const fc::ImageStyle& o)
                                             {
                                                 for (const auto& role : fc::imageRoles()) if (o.*(role.field) != st.*(role.field)) return false;
                                                 return true;
                                             });
            if (known != f.styles.end()) { selectRow = static_cast<int> (known - f.styles.begin()); continue; }
            if (static_cast<int> (f.styles.size()) >= fc::maxStyles) break;
            st.id = f.freeStyleId();
            f.styles.push_back (std::move (st));
            selectRow = static_cast<int> (f.styles.size()) - 1;
            ++added;
        }
        if (added > 0) owner_.commit (std::move (f), "Facade: add styles from a pack");
        owner_.canvas_->clearPictures();
        list_.updateContent();
        if (selectRow >= 0) list_.selectRow (selectRow);
        syncFields();
        return added;
    }

    // Menu de la bibliothèque : styles des packs installés, installation, dossier, export.
    void showLibraryMenu()
    {
        auto packs = std::make_shared<std::vector<stylelib::Pack>> (stylelib::scan());
        juce::PopupMenu menu;
        for (std::size_t p = 0; p < packs->size(); ++p)
        {
            const auto& info = (*packs)[p].info;
            juce::PopupMenu sub;
            for (std::size_t i = 0; i < info.styles.size(); ++i) sub.addItem (1000 + static_cast<int> (p) * 100 + static_cast<int> (i), utf8 (info.styles[i].style.name));
            menu.addSubMenu (utf8 (info.name) + (info.author.empty() ? juce::String() : "  (" + utf8 (info.author) + ")"), sub, ! info.styles.empty());
        }
        if (packs->empty()) menu.addItem (-1, owner_.text ("facade.lib_empty"), false);
        menu.addSeparator();
        menu.addItem (1, owner_.text ("facade.lib_install"));
        menu.addItem (2, owner_.text ("facade.lib_open_folder"));
        menu.addItem (3, owner_.text ("facade.lib_export"), current() != nullptr);
        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&libraryButton_), [safe = juce::Component::SafePointer<StylePanel> (this), packs] (int choice)
        {
            if (safe == nullptr || choice == 0) return;
            if (choice >= 1000)
            {
                const auto p = static_cast<std::size_t> ((choice - 1000) / 100);
                if (p < packs->size()) safe->addPackStyles ((*packs)[p], (choice - 1000) % 100);
            }
            else if (choice == 1) safe->chooseAndInstall();
            else if (choice == 2) { stylelib::directory().createDirectory(); stylelib::directory().revealToUser(); }
            else if (choice == 3) safe->exportCurrent();
        });
    }

    void chooseAndInstall()
    {
        chooser_ = std::make_unique<juce::FileChooser> (owner_.text ("facade.lib_choose_pack"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.neptunestyle");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [safe = juce::Component::SafePointer<StylePanel> (this)] (const juce::FileChooser& c)
                               {
                                   if (safe == nullptr || ! c.getResult().existsAsFile()) return;
                                   safe->setMessage (owner_message (safe->owner_, c.getResult()));
                               });
    }

    // Installe un pack et rend le message à afficher.
    static juce::String owner_message (FacadeView& owner, const juce::File& file)
    {
        juce::String error;
        int count = 0;
        const auto name = stylelib::install (file, error, &count);
        if (name.isEmpty()) return juce::String::fromUTF8 (owner.tr_.format ("facade.lib_error", { error.toStdString() }).c_str());
        return juce::String::fromUTF8 (owner.tr_.format ("facade.lib_installed", { name.toStdString(), std::to_string (count) }).c_str());
    }

    void exportCurrent()
    {
        const auto* st = current();
        if (st == nullptr) return;
        const auto style = *st;
        chooser_ = std::make_unique<juce::FileChooser> (owner_.text ("facade.lib_save_pack"),
                                                        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (utf8 (style.name) + ".neptunestyle"), "*.neptunestyle");
        chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                               [safe = juce::Component::SafePointer<StylePanel> (this), style] (const juce::FileChooser& c)
                               {
                                   if (safe == nullptr || c.getResult() == juce::File()) return;
                                   auto target = c.getResult();
                                   if (! target.hasFileExtension ("neptunestyle")) target = target.withFileExtension ("neptunestyle");
                                   const bool ok = stylelib::exportStyle (style, target);
                                   safe->setMessage (juce::String::fromUTF8 (safe->owner_.tr_.format (ok ? "facade.lib_exported" : "facade.lib_export_failed", { target.getFullPathName().toStdString() }).c_str()));
                               });
    }

    // Écrit les modèles (un dossier par style, pack.json, SPECIFICATION.md) dans un dossier au choix, en fait aussi un pack installé dans la
    // bibliothèque, et ajoute ses styles à la Façade.
    void createExamples()
    {
        chooser_ = std::make_unique<juce::FileChooser> (owner_.text ("facade.examples_choose"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [safe = juce::Component::SafePointer<StylePanel> (this)] (const juce::FileChooser& c)
                               {
                                   if (safe == nullptr) return;
                                   const auto dir = c.getResult();
                                   if (dir.isDirectory()) safe->writeExamples (dir.getChildFile ("Neptune-styles"));
                               });
    }

    void writeExamples (const juce::File& dir)
    {
        if (! dir.createDirectory().wasOk()) return;
        const bool french = owner_.tr_.language() == "fr";
        int written = 0;
        auto files = facade_examples::files (french);
        files.push_back ({ "SPECIFICATION.md", french ? facade_spec::french() : facade_spec::english() });
        for (const auto& f : files)
        {
            const auto target = dir.getChildFile (f.name);
            if (target.existsAsFile() && f.name != "SPECIFICATION.md") continue;   // un modèle déjà modifié n'est jamais écrasé
            target.getParentDirectory().createDirectory();
            if (target.replaceWithText (f.content)) ++written;
        }
        // le même contenu, en archive installée dans la bibliothèque : de quoi essayer les styles tout de suite
        const auto lib = stylelib::directory();
        int added = 0;
        if (lib.createDirectory().wasOk())
        {
            juce::ZipFile::Builder builder;
            for (const auto& f : facade_examples::files (french))
                builder.addEntry (new juce::MemoryInputStream (f.content.toRawUTF8(), f.content.getNumBytesAsUTF8(), true), 9, f.name, juce::Time::getCurrentTime());
            const auto archive = lib.getChildFile ("neptune-examples.neptunestyle");
            archive.deleteFile();
            juce::FileOutputStream out (archive);
            if (out.openedOk() && builder.writeToStream (out, nullptr))
            {
                out.flush();
                stylelib::Pack pack;
                if (stylelib::analyseSource (archive, pack)) added = addPackStyles (pack);
            }
        }
        hint_.setText (juce::String::fromUTF8 (owner_.tr_.format ("facade.examples_done", { dir.getFullPathName().toStdString(), std::to_string (written), std::to_string (added) }).c_str()), juce::dontSendNotification);
    }

    void createStyle()
    {
        auto f = owner_.facade_;
        if (static_cast<int> (f.styles.size()) >= fc::maxStyles) return;
        fc::ImageStyle st;
        st.id = f.freeStyleId();
        st.name = owner_.text ("facade.style_default_name").toStdString() + " " + std::to_string (st.id);
        f.styles.push_back (st);
        const int row = static_cast<int> (f.styles.size()) - 1;
        owner_.commit (std::move (f), "Facade: add style");
        list_.updateContent();
        list_.selectRow (row);
        syncFields();
    }

    void deleteStyle()
    {
        const int row = list_.getSelectedRow();
        auto f = owner_.facade_;
        if (row < 0 || row >= static_cast<int> (f.styles.size())) return;
        f.styles.erase (f.styles.begin() + row);
        owner_.commit (std::move (f), "Facade: delete style");
        owner_.canvas_->clearPictures();
        list_.updateContent();
        list_.selectRow (juce::jmin (row, static_cast<int> (owner_.facade_.styles.size()) - 1));
        syncFields();
    }

    void close()
    {
        commitName();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->stylePanel_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    int getNumRows() override { return static_cast<int> (owner_.facade_.styles.size()); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        const auto& styles = owner_.facade_.styles;
        if (row < 0 || row >= static_cast<int> (styles.size())) return;
        if (selected) { g.setColour (owner_.colour (Token::Accent).withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
        g.setColour (owner_.colour (Token::Text));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (utf8 (styles[static_cast<std::size_t> (row)].name), 8, 0, w - 16, h, juce::Justification::centredLeft);
    }
    void selectedRowsChanged (int) override { syncFields(); }

    FacadeView& owner_;
    juce::Label title_, nameLabel_, hint_, imagesCaption_, optionsCaption_, positionLabel_;
    juce::ListBox list_;
    juce::TextButton newButton_, deleteButton_, closeButton_, examplesButton_, libraryButton_;
    juce::TextEditor name_;
    juce::ToggleButton stripH_, bold_;
    juce::ComboBox position_;
    juce::Component content_;
    juce::Viewport view_;
    std::vector<std::unique_ptr<ImageRow>> imageRows_;
    std::vector<std::unique_ptr<IntRow>> intRows_;
    std::vector<std::unique_ptr<TextRow>> textRows_;
    int contentHeight_ = 0;
    bool syncing_ = false;
    Preview preview_ { *this };
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StylePanel)
};

// ============================== Crans d'un potard ou d'un curseur ==============================

// Fonction d'un potard ou d'un curseur, indépendante de son style : automatique, continu, ou crans personnalisés (une valeur par ligne).
class FacadeView::NotchPanel : public juce::Component
{
public:
    NotchPanel (FacadeView& owner, int widgetId) : owner_ (owner), id_ (widgetId)
    {
        const auto* w = owner.facade_.find (widgetId);
        auto& ps = owner.processor_.parameters();
        spec_ = w != nullptr && w->param != 0 ? ps.find (w->param) : nullptr;
        title_.setText (owner.text ("facade.notch_title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        const char* keys[3] = { "facade.notch_auto", "facade.notch_continuous", "facade.notch_custom" };
        for (int i = 0; i < 3; ++i)
        {
            modes_[static_cast<std::size_t> (i)].setButtonText (owner.text (keys[i]));
            modes_[static_cast<std::size_t> (i)].setRadioGroupId (4242);
            modes_[static_cast<std::size_t> (i)].setColour (juce::ToggleButton::textColourId, owner.colour (Token::Text));
            addAndMakeVisible (modes_[static_cast<std::size_t> (i)]);
        }
        modes_[static_cast<std::size_t> (w != nullptr ? juce::jlimit (0, 2, w->stepMode) : 0)].setToggleState (true, juce::dontSendNotification);
        hint_.setText (owner.text (spec_ != nullptr ? "facade.notch_hint" : "facade.notch_unlinked"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
        addAndMakeVisible (hint_);
        editor_.setMultiLine (true, false);
        editor_.setReturnKeyStartsNewLine (true);
        editor_.setFont (juce::FontOptions (17.0f));
        if (w != nullptr && spec_ != nullptr)
        {
            juce::StringArray lines;
            for (const double v : w->notches) lines.add (utf8 (ps.toText (*spec_, v)));
            editor_.setText (lines.joinIntoString ("\n"), false);
        }
        editor_.setEnabled (spec_ != nullptr);
        addAndMakeVisible (editor_);
        countLabel_.setText (owner.text ("facade.notch_count"), juce::dontSendNotification);
        countLabel_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (countLabel_);
        count_.setInputRestrictions (3, "0123456789");
        count_.setFont (juce::FontOptions (17.0f));
        count_.setText ("9", false);
        addAndMakeVisible (count_);
        spread_.setButtonText (owner.text ("facade.notch_spread"));
        spread_.setEnabled (spec_ != nullptr);
        spread_.onClick = [this]
        {
            if (spec_ == nullptr) return;
            juce::StringArray lines;
            for (const double v : fc::evenNotches (*spec_, count_.getText().getIntValue())) lines.add (utf8 (owner_.processor_.parameters().toText (*spec_, v)));
            editor_.setText (lines.joinIntoString ("\n"), false);
            modes_[2].setToggleState (true, juce::dontSendNotification);
        };
        addAndMakeVisible (spread_);
        error_.setColour (juce::Label::textColourId, owner.colour (Token::KnobHot));
        addAndMakeVisible (error_);
        ok_.setButtonText (owner.text ("facade.ok"));
        ok_.onClick = [this] { accept(); };
        cancel_.setButtonText (owner.text ("facade.picker_cancel"));
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible (ok_);
        addAndMakeVisible (cancel_);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        for (auto& m : modes_) m.setBounds (r.removeFromTop (28));
        hint_.setBounds (r.removeFromTop (48));
        auto bottom = r.removeFromBottom (44);
        r.removeFromBottom (6);
        error_.setBounds (r.removeFromBottom (24));
        auto row = r.removeFromBottom (34);
        r.removeFromBottom (6);
        editor_.setBounds (r);
        countLabel_.setBounds (row.removeFromLeft (110));
        count_.setBounds (row.removeFromLeft (70));
        row.removeFromLeft (10);
        spread_.setBounds (row.removeFromLeft (260));
        cancel_.setBounds (bottom.removeFromRight (150));
        bottom.removeFromRight (10);
        ok_.setBounds (bottom.removeFromRight (150));
    }

private:
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (560, juce::jmin (600, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void accept()
    {
        int mode = 0;
        for (int i = 0; i < 3; ++i) if (modes_[static_cast<std::size_t> (i)].getToggleState()) mode = i;
        std::vector<double> notches;
        if (mode == 2)
        {
            if (spec_ == nullptr) { error_.setText (owner_.text ("facade.notch_unlinked"), juce::dontSendNotification); return; }
            const auto lines = juce::StringArray::fromLines (editor_.getText());
            for (int i = 0; i < lines.size(); ++i)
            {
                if (lines[i].trim().isEmpty()) continue;
                double plain = 0.0;
                if (! owner_.processor_.parameters().fromText (spec_->id, lines[i].trim().toStdString(), plain))
                {
                    error_.setText (juce::String::fromUTF8 (owner_.tr_.format ("facade.notch_error_line", { std::to_string (i + 1) }).c_str()), juce::dontSendNotification);
                    return;
                }
                notches.push_back (spec_->clamp (plain));
            }
            if (notches.size() < 2) { error_.setText (owner_.text ("facade.notch_error_few"), juce::dontSendNotification); return; }
        }
        auto f = owner_.facade_;
        if (auto* w = f.find (id_))
        {
            w->stepMode = mode;
            if (mode == 2) w->notches = notches;
            owner_.commit (std::move (f), "Facade: notches");
        }
        close();
    }

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->notchPanel_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    FacadeView& owner_;
    int id_;
    const neptune::core::ParamSpec* spec_ = nullptr;
    juce::Label title_, hint_, countLabel_, error_;
    std::array<juce::ToggleButton, 3> modes_;
    juce::TextEditor editor_, count_;
    juce::TextButton spread_, ok_, cancel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotchPanel)
};

// Échelle et courbe libre du lien d'un potard ou d'un curseur : comment la position du widget devient la valeur du paramètre.
// Travaille sur une copie ; OK applique en un seul geste annulable, Annuler jette.
class FacadeView::ScalePanel : public juce::Component
{
public:
    ScalePanel (FacadeView& owner, int widgetId) : owner_ (owner), id_ (widgetId)
    {
        const auto* w = owner.facade_.find (widgetId);
        scale_ = w != nullptr ? w->scale : 0;
        curve_ = w != nullptr ? w->response : neptune::core::VelocityCurve();
        const auto* spec = w != nullptr && w->param != 0 ? owner.processor_.parameters().find (w->param) : nullptr;
        applicable_ = spec != nullptr && spec->kind == neptune::core::Kind::Continuous;
        title_.setText (owner.text ("facade.scale_title"), juce::dontSendNotification);
        title_.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        title_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (title_);
        const char* keys[fc::numWidgetScales] = { "facade.scale_param", "facade.scale_linear", "facade.scale_log", "facade.scale_exp" };
        for (int i = 0; i < fc::numWidgetScales; ++i)
        {
            auto& b = modes_[static_cast<std::size_t> (i)];
            b.setButtonText (owner.text (keys[i]));
            b.setRadioGroupId (4343);
            b.setColour (juce::ToggleButton::textColourId, owner.colour (Token::Text));
            b.setEnabled (applicable_);
            b.onClick = [this, i] { scale_ = i; };
            addAndMakeVisible (b);
        }
        modes_[static_cast<std::size_t> (juce::jlimit (0, fc::numWidgetScales - 1, scale_))].setToggleState (true, juce::dontSendNotification);
        hint_.setText (owner.text (applicable_ ? "facade.scale_hint" : "facade.scale_unavailable"), juce::dontSendNotification);
        hint_.setFont (juce::FontOptions (13.0f));
        hint_.setColour (juce::Label::textColourId, owner.colour (Token::TextDim));
        addAndMakeVisible (hint_);
        curveLabel_.setText (owner.text ("facade.scale_curve"), juce::dontSendNotification);
        curveLabel_.setColour (juce::Label::textColourId, owner.colour (Token::Text));
        addAndMakeVisible (curveLabel_);
        editor_ = std::make_unique<VelocityPanel> (owner.palette_, owner.tr_, 0, false, curve_);
        editor_->setFadeControlsVisible (false);
        editor_->setEnabled (applicable_);
        editor_->onChange = [this] (int, bool, const neptune::core::VelocityCurve& c) { curve_ = c; };
        addAndMakeVisible (*editor_);
        ok_.setButtonText (owner.text ("facade.ok"));
        ok_.setEnabled (applicable_);
        ok_.onClick = [this] { accept(); };
        cancel_.setButtonText (owner.text ("facade.picker_cancel"));
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible (ok_);
        addAndMakeVisible (cancel_);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (owner_.colour (Token::PanelBg));
        g.fillRoundedRectangle (panel().toFloat(), 10.0f);
        g.setColour (owner_.colour (Token::Border));
        g.drawRoundedRectangle (panel().toFloat(), 10.0f, 1.5f);
    }

    void resized() override
    {
        auto r = panel().reduced (18);
        title_.setBounds (r.removeFromTop (32));
        for (auto& m : modes_) m.setBounds (r.removeFromTop (28));
        hint_.setBounds (r.removeFromTop (48));
        curveLabel_.setBounds (r.removeFromTop (26));
        auto bottom = r.removeFromBottom (44);
        r.removeFromBottom (6);
        editor_->setBounds (r);
        cancel_.setBounds (bottom.removeFromRight (150));
        bottom.removeFromRight (10);
        ok_.setBounds (bottom.removeFromRight (150));
    }

private:
    juce::Rectangle<int> panel() const { return juce::Rectangle<int> (600, juce::jmin (640, getHeight() - 20)).withCentre (getLocalBounds().getCentre()); }

    void accept()
    {
        auto f = owner_.facade_;
        if (auto* w = f.find (id_))
        {
            w->scale = scale_;
            w->response = curve_;
            owner_.commit (std::move (f), "Facade: scale");
        }
        close();
    }

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (&owner_)]
        {
            if (safe != nullptr) { safe->scalePanel_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    }

    FacadeView& owner_;
    int id_;
    int scale_ = 0;
    bool applicable_ = false;
    neptune::core::VelocityCurve curve_;
    juce::Label title_, hint_, curveLabel_;
    std::array<juce::ToggleButton, fc::numWidgetScales> modes_;
    std::unique_ptr<VelocityPanel> editor_;
    juce::TextButton ok_, cancel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScalePanel)
};

// ============================== Vue ==============================

FacadeView::FacadeView (NeptuneProcessor& p, const neptune::core::theme::Palette& palette, const neptune::core::i18n::Translator& tr,
                        neptune::core::theme::KnobStyle style, NeptuneKnob::OpenModal openModal)
    : processor_ (p), palette_ (palette), tr_ (tr), knobStyle_ (style), openModal_ (std::move (openModal))
{
    stylelib::setDirectory (juce::File (processor_.settingsFilePath()).getParentDirectory().getChildFile ("styles"));
    canvas_ = std::make_unique<Canvas> (*this);
    addAndMakeVisible (*canvas_);

    scopeTrackButton_.setButtonText (text ("facade.scope_track"));
    scopeTrackButton_.setTooltip (text ("facade.tip_scope_track"));
    scopeGlobalButton_.setButtonText (text ("facade.scope_global"));
    scopeGlobalButton_.setTooltip (text ("facade.tip_scope_global"));
    for (auto* b : { &scopeTrackButton_, &scopeGlobalButton_ })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (5871);
        addAndMakeVisible (*b);
    }
    scopeTrackButton_.setToggleState (true, juce::dontSendNotification);
    scopeTrackButton_.onClick = [this] { if (scopeTrackButton_.getToggleState()) setGlobal (false); };
    scopeGlobalButton_.onClick = [this] { if (scopeGlobalButton_.getToggleState()) setGlobal (true); };
    addPageButton_.setButtonText (text ("facade.add_page"));
    addPageButton_.setTooltip (text ("facade.tip_add_page"));
    addPageButton_.onClick = [this]
    {
        if (! hasTarget() || fc::pageCount (book_) >= fc::maxPages) return;
        writeBook (fc::addPage (book_, ""), "Facade: add a page");
        setPage (fc::pageCount (book_) - 1);
    };
    pageMenuButton_.setButtonText (text ("facade.page_menu"));
    pageMenuButton_.setTooltip (text ("facade.tip_page_menu"));
    pageMenuButton_.onClick = [this] { showPageMenu(); };
    addChildComponent (addPageButton_);    // visibles seulement en mode Édition (updateButtons)
    addChildComponent (pageMenuButton_);
    editButton_.setButtonText (text ("facade.edit_layout"));
    editButton_.setClickingTogglesState (true);
    editButton_.onClick = [this]
    {
        canvas_->releaseKeys();
        editMode_ = editButton_.getToggleState();
        if (! editMode_) selection_.clear();
        updateButtons();
        resized();
        canvas_->syncChildren();
        canvas_->repaint();
    };
    auto setupAdd = [this] (juce::TextButton& b, const char* key, fc::WidgetType type)
    {
        b.setButtonText (text (key));
        b.onClick = [this, type] { addWidget (type); };
    };
    setupAdd (knobButton_, "facade.add_knob", fc::WidgetType::Knob);
    setupAdd (buttonButton_, "facade.add_button", fc::WidgetType::Button);
    setupAdd (menuButton_, "facade.add_menu", fc::WidgetType::Menu);
    setupAdd (actionButton_, "facade.add_action", fc::WidgetType::Action);
    setupAdd (curveButton_, "facade.add_curve", fc::WidgetType::Curve);
    setupAdd (labelButton_, "facade.add_label", fc::WidgetType::Label);
    setupAdd (zoneButton_, "facade.add_zone", fc::WidgetType::Zone);
    setupAdd (keyboardButton_, "facade.add_keyboard", fc::WidgetType::Keyboard);
    setupAdd (lampButton_, "facade.add_lamp", fc::WidgetType::Lamp);
    setupAdd (setKeyButton_, "facade.add_set", fc::WidgetType::Set);
    setupAdd (midiButton_, "facade.add_midi", fc::WidgetType::Midi);
    setupAdd (meterButton_, "facade.add_meter", fc::WidgetType::Meter);
    setupAdd (gridButton_, "facade.add_grid", fc::WidgetType::SceneGrid);
    gridOptionsButton_.setButtonText (text ("facade.grid_options"));
    gridOptionsButton_.setTooltip (text ("facade.tip_grid_options"));
    gridOptionsButton_.onClick = [this] { showGridMenu(); };
    sideHideButton_.setButtonText (text ("facade.hide_bar"));
    sideHideButton_.setTooltip (text ("facade.tip_hide_bar"));
    sideHideButton_.onClick = [this] { sideHidden_ = true; resized(); repaint(); };
    sideShowButton_.setButtonText (">");
    sideShowButton_.setTooltip (text ("facade.tip_show_bar"));
    sideShowButton_.onClick = [this] { sideHidden_ = false; resized(); repaint(); };
    addChildComponent (sideHideButton_);
    addChildComponent (sideShowButton_);
    meterSourceButton_.setButtonText (text ("facade.meter_source"));
    meterSourceButton_.setTooltip (text ("facade.tip_meter_source"));
    meterSourceButton_.onClick = [this] { showMeterSourceMenu(); };
    midiEditButton_.setButtonText (text ("facade.midi_edit"));
    midiEditButton_.setTooltip (text ("facade.tip_midi_edit"));
    midiEditButton_.onClick = [this] { openMidiEditor(); };
    actionModeButton_.setButtonText (text ("facade.action_mode"));
    actionModeButton_.setTooltip (text ("facade.tip_action_mode"));
    actionModeButton_.onClick = [this] { showActionModeMenu(); };
    lampSourceButton_.setButtonText (text ("facade.lamp_source"));
    lampSourceButton_.setTooltip (text ("facade.tip_lamp_source"));
    lampSourceButton_.onClick = [this] { showLampSourceMenu(); };
    setupAdd (sliderButton_, "facade.add_slider", fc::WidgetType::Slider);
    notchesButton_.setButtonText (text ("facade.notches"));
    notchesButton_.setTooltip (text ("facade.tip_notches"));
    notchesButton_.onClick = [this] { openNotches(); };
    scaleButton_.setButtonText (text ("facade.scale"));
    scaleButton_.setTooltip (text ("facade.tip_scale"));
    scaleButton_.onClick = [this] { openScale(); };
    invertButton_.setButtonText (text ("facade.invert"));
    invertButton_.setTooltip (text ("facade.tip_invert"));
    invertButton_.onClick = [this] { applySingle ([] (fc::Widget& w) { w.invert = w.invert != 0 ? 0 : 1; }, "Facade: invert"); };
    saveFacadeButton_.setButtonText (text ("facade.save_facade"));
    saveFacadeButton_.setTooltip (text ("facade.tip_save_facade"));
    saveFacadeButton_.onClick = [this] { saveFacadeFile(); };
    loadFacadeButton_.setButtonText (text ("facade.load_facade"));
    loadFacadeButton_.setTooltip (text ("facade.tip_load_facade"));
    loadFacadeButton_.onClick = [this] { loadFacadeFile(); };
    relocateButton_.setButtonText (text ("facade.relocate"));
    relocateButton_.setTooltip (text ("facade.tip_relocate"));
    relocateButton_.onClick = [this] { relocate(); };
    stylesButton_.setButtonText (text ("facade.styles"));
    stylesButton_.setTooltip (text ("facade.tip_styles"));
    stylesButton_.onClick = [this] { openStyles(); };
    styleButton_.setButtonText (text ("facade.style"));
    styleButton_.setTooltip (text ("facade.tip_style"));
    styleButton_.onClick = [this] { showStyleMenu(); };
    orientButton_.setButtonText (text ("facade.vertical"));
    orientButton_.setTooltip (text ("facade.tip_vertical"));
    orientButton_.onClick = [this] { applySingle ([] (fc::Widget& w) { w.vertical = w.vertical != 0 ? 0 : 1; std::swap (w.w, w.h); }, "Facade: slider orientation"); };
    imageButton_.setButtonText (text ("facade.image"));
    imageButton_.onClick = [this] { chooseImage(); };
    fitButton_.setButtonText (text ("facade.fit_fit"));
    fitButton_.onClick = [this] { showFitMenu(); };
    clearImageButton_.setButtonText (text ("facade.image_clear"));
    clearImageButton_.onClick = [this]
    {
        auto f = facade_;
        f.image.clear();
        commit (std::move (f), "Facade: background");
    };
    linkButton_.setButtonText (text ("facade.link"));
    linkButton_.onClick = [this] { openPicker(); };
    textButton_.setButtonText (text ("facade.text"));
    textButton_.onClick = [this] { editText(); };
    captureButton_.setButtonText (text ("facade.capture"));
    captureButton_.onClick = [this] { openPicker (true); };
    duplicateButton_.setButtonText (text ("facade.duplicate"));
    duplicateButton_.onClick = [this] { duplicateSelected(); };
    deleteButton_.setButtonText (text ("facade.delete"));
    deleteButton_.onClick = [this] { deleteSelected(); };
    alignButton_.setButtonText (text ("facade.align"));
    alignButton_.onClick = [this] { showAlignMenu(); };
    sameSizeButton_.setButtonText (text ("facade.same_size"));
    sameSizeButton_.setTooltip (text ("facade.tip_same_size"));
    sameSizeButton_.onClick = [this]
    {
        auto f = facade_;
        fc::matchSize (f, single());
        std::set<int> all;
        for (const auto& w : f.widgets) all.insert (w.id);
        commitArranged (std::move (f), all, "Facade: same size");
    };
    layoutButton_.setButtonText (text ("facade.zone_layout"));
    layoutButton_.setTooltip (text ("facade.tip_zone_layout"));
    layoutButton_.onClick = [this] { showZoneLayoutMenu(); };
    zoneAlignButton_.setButtonText (text ("facade.zone_align"));
    zoneAlignButton_.setTooltip (text ("facade.tip_zone_align"));
    zoneAlignButton_.onClick = [this] { showZoneAlignMenu(); };
    zoneGapButton_.setButtonText (text ("facade.zone_gap"));
    zoneGapButton_.setTooltip (text ("facade.tip_zone_gap"));
    zoneGapButton_.onClick = [this] { showZoneGapMenu(); };
    kbChannelButton_.setButtonText (text ("facade.kb_channel"));
    kbChannelButton_.setTooltip (text ("facade.tip_kb_channel"));
    kbChannelButton_.onClick = [this] { showKeyboardMenu (0); };
    kbFirstButton_.setButtonText (text ("facade.kb_first"));
    kbFirstButton_.setTooltip (text ("facade.tip_kb_first"));
    kbFirstButton_.onClick = [this] { showKeyboardMenu (1); };
    kbVelocityButton_.setButtonText (text ("facade.kb_velocity"));
    kbVelocityButton_.setTooltip (text ("facade.tip_kb_velocity"));
    kbVelocityButton_.onClick = [this] { showKeyboardMenu (2); };
    kbHoldButton_.setButtonText (text ("facade.kb_hold"));
    kbHoldButton_.setTooltip (text ("facade.tip_kb_hold"));
    kbHoldButton_.onClick = [this] { applySingle ([] (fc::Widget& w) { w.hold = w.hold != 0 ? 0 : 1; }, "Facade: keyboard hold"); };
    kbWheelsButton_.setButtonText (text ("facade.kb_wheels"));
    kbWheelsButton_.setTooltip (text ("facade.tip_kb_wheels"));
    kbWheelsButton_.setClickingTogglesState (false);
    kbWheelsButton_.onClick = [this] { applySingle ([] (fc::Widget& w) { w.wheels = w.wheels != 0 ? 0 : 1; }, "Facade: keyboard wheels"); };
    helpButton_.setButtonText ("?");
    helpButton_.setTooltip (text ("facade.help_title"));
    helpButton_.onClick = [this] { openHelp(); };
    editButton_.setTooltip (text ("facade.tip_edit"));
    knobButton_.setTooltip (text ("facade.tip_knob"));
    buttonButton_.setTooltip (text ("facade.tip_button"));
    menuButton_.setTooltip (text ("facade.tip_menu"));
    actionButton_.setTooltip (text ("facade.tip_action"));
    curveButton_.setTooltip (text ("facade.tip_curve"));
    zoneButton_.setTooltip (text ("facade.tip_zone"));
    sliderButton_.setTooltip (text ("facade.tip_slider"));
    keyboardButton_.setTooltip (text ("facade.tip_keyboard"));
    lampButton_.setTooltip (text ("facade.tip_lamp"));
    setKeyButton_.setTooltip (text ("facade.tip_set"));
    midiButton_.setTooltip (text ("facade.tip_midi"));
    meterButton_.setTooltip (text ("facade.tip_meter"));
    gridButton_.setTooltip (text ("facade.tip_grid"));
    labelButton_.setTooltip (text ("facade.tip_label"));
    captureButton_.setTooltip (text ("facade.tip_capture"));
    alignButton_.setTooltip (text ("facade.tip_align"));

    for (auto* b : { &editButton_, &knobButton_, &buttonButton_, &menuButton_, &actionButton_, &curveButton_, &labelButton_, &zoneButton_, &keyboardButton_, &lampButton_, &lampSourceButton_, &setKeyButton_, &actionModeButton_, &midiButton_, &midiEditButton_, &meterButton_, &meterSourceButton_, &gridButton_, &gridOptionsButton_, &imageButton_, &fitButton_, &clearImageButton_,
                     &sliderButton_, &stylesButton_, &styleButton_, &orientButton_, &notchesButton_, &scaleButton_, &invertButton_, &saveFacadeButton_, &loadFacadeButton_, &relocateButton_, &kbChannelButton_, &kbFirstButton_, &kbHoldButton_, &kbWheelsButton_, &kbVelocityButton_, &sameSizeButton_, &layoutButton_, &zoneAlignButton_, &zoneGapButton_, &linkButton_, &textButton_, &captureButton_, &duplicateButton_, &deleteButton_, &alignButton_, &helpButton_ })
        addAndMakeVisible (*b);
    refresh();
    updateButtons();
    startTimerHz (15);
}

FacadeView::~FacadeView() { stopTimer(); }

juce::Colour FacadeView::colour (Token t) const { return toColour (palette_, t); }

void FacadeView::pruneSelection()
{
    for (auto it = selection_.begin(); it != selection_.end();)
        it = facade_.find (*it) != nullptr ? std::next (it) : selection_.erase (it);
}

void FacadeView::updateButtons()
{
    pruneSelection();
    const auto* one = facade_.find (single());
    for (auto* b : { &knobButton_, &buttonButton_, &menuButton_, &actionButton_, &curveButton_, &labelButton_, &zoneButton_, &keyboardButton_, &lampButton_, &setKeyButton_, &midiButton_, &meterButton_, &gridButton_, &sliderButton_, &stylesButton_, &imageButton_, &fitButton_, &clearImageButton_, &saveFacadeButton_, &loadFacadeButton_ }) b->setVisible (editMode_);
    relocateButton_.setVisible (missing_ > 0);
    addPageButton_.setVisible (editMode_);
    pageMenuButton_.setVisible (editMode_);
    const bool any = editMode_ && ! selection_.empty();
    textButton_.setVisible (editMode_ && one != nullptr);
    linkButton_.setVisible (editMode_ && one != nullptr && needsParam (one->type) && ! (one->type == fc::WidgetType::Lamp && one->source != 0));
    lampSourceButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::Lamp);
    captureButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::Action);
    actionModeButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::Action);
    midiEditButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::Midi);
    meterSourceButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::Meter);
    gridOptionsButton_.setVisible (editMode_ && one != nullptr && one->type == fc::WidgetType::SceneGrid);
    duplicateButton_.setVisible (any);
    deleteButton_.setVisible (any);
    alignButton_.setVisible (editMode_ && selection_.size() >= 2);
    sameSizeButton_.setVisible (editMode_ && one != nullptr);
    const bool zone = editMode_ && one != nullptr && one->type == fc::WidgetType::Zone;
    const bool keyboard = editMode_ && one != nullptr && one->type == fc::WidgetType::Keyboard;
    for (auto* b : { &kbChannelButton_, &kbFirstButton_, &kbHoldButton_, &kbWheelsButton_, &kbVelocityButton_ }) b->setVisible (keyboard);
    kbChannelButton_.setVisible (keyboard || (editMode_ && one != nullptr && one->type == fc::WidgetType::Lamp && one->source == 1));   // un voyant de notes lit aussi un canal
    bool anyStylable = false;
    for (const auto& w : facade_.widgets)
        if (selection_.count (w.id) != 0 && takesStyle (w.type)) anyStylable = true;
    styleButton_.setVisible (editMode_ && anyStylable);
    orientButton_.setVisible (editMode_ && one != nullptr && (one->type == fc::WidgetType::Slider || one->type == fc::WidgetType::Meter));
    const bool dial = editMode_ && one != nullptr && (one->type == fc::WidgetType::Slider || one->type == fc::WidgetType::Knob);
    notchesButton_.setVisible (dial);
    scaleButton_.setVisible (dial);
    invertButton_.setVisible (dial);
    if (dial) invertButton_.setToggleState (one->invert != 0, juce::dontSendNotification);
    if (one != nullptr && (one->type == fc::WidgetType::Slider || one->type == fc::WidgetType::Meter)) orientButton_.setToggleState (one->vertical != 0, juce::dontSendNotification);
    if (keyboard) { kbHoldButton_.setToggleState (one->hold != 0, juce::dontSendNotification); kbWheelsButton_.setToggleState (one->wheels != 0, juce::dontSendNotification); }
    layoutButton_.setVisible (zone);
    zoneAlignButton_.setVisible (zone);
    zoneGapButton_.setVisible (zone);
    clearImageButton_.setEnabled (! facade_.image.empty());
    fitButton_.setButtonText (text (facade_.fit == fc::ImageFit::Fit ? "facade.fit_fit" : facade_.fit == fc::ImageFit::Stretch ? "facade.fit_stretch" : "facade.fit_fill"));
    editButton_.setToggleState (editMode_, juce::dontSendNotification);
    resized();   // les boutons visibles se partagent la rangée
}

void FacadeView::resized()
{
    static constexpr int rowH = 28, gap = 6, pad = 24;
    auto fit = [this] (const juce::TextButton& b)
    {
        return juce::roundToInt (std::ceil (getLookAndFeel().getTextButtonFont (const_cast<juce::TextButton&> (b), rowH).getStringWidthFloat (b.getButtonText()))) + pad;
    };
    const auto whole = getLocalBounds();
    // Jeu : la barre d'onglets passe dans une colonne à gauche quand la Façade y gagne (écran large : la hauteur compte), sinon elle reste en haut ; elle peut se replier.
    std::vector<juce::TextButton*> sideItems;
    for (auto* b : { &scopeTrackButton_, &scopeGlobalButton_ }) sideItems.push_back (b);
    for (auto& tab : pageTabs_) sideItems.push_back (tab.get());
    for (auto* b : { &editButton_, &relocateButton_, &helpButton_, &sideHideButton_ }) sideItems.push_back (b);
    bool sideLayout = false;
    int sideW = 120;
    if (! editMode_)
    {
        sideHideButton_.setVisible (! sideHidden_);
        sideShowButton_.setVisible (sideHidden_);
        for (auto* b : sideItems)
            if (b->isVisible() && ! sideHidden_) sideW = std::max (sideW, std::min (230, fit (*b)));
        const float ratio = static_cast<float> (fc::canvasWidth) / static_cast<float> (fc::canvasHeight);
        const float topBarH = static_cast<float> (2 * (rowH + gap) + 24);
        const float canvasWithColumn = std::min (static_cast<float> (whole.getWidth() - sideW - 28), ratio * static_cast<float> (whole.getHeight() - 12));
        const float canvasWithTopBar = std::min (static_cast<float> (whole.getWidth()), ratio * (static_cast<float> (whole.getHeight()) - topBarH));
        sideLayout = canvasWithColumn >= canvasWithTopBar;
    }
    const bool collapsed = ! editMode_ && sideHidden_;
    if (collapsed != lastCollapsed_)
    {
        lastCollapsed_ = collapsed;
        if (onCollapsedChanged) juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FacadeView> (this)] { if (safe != nullptr && safe->onCollapsedChanged) safe->onCollapsedChanged(); });
    }
    if (collapsed || sideLayout)
    {
        int y = whole.getY() + 12;
        for (auto* b : sideItems)
        {
            if (collapsed || ! b->isVisible()) { if (b != &sideHideButton_) b->setBounds ({}); continue; }
            b->setBounds (whole.getX() + 12, y, sideW, rowH);
            y += rowH + gap;
        }
        if (collapsed)
        {
            for (auto* b : sideItems) b->setBounds ({});
            sideShowButton_.setBounds (whole.getX(), whole.getY(), 26, 24);   // un petit carré collé au coin haut gauche, contre la barre du haut
            sideShowButton_.toFront (false);
            toolbarRect_ = {};
            canvas_->setBounds (whole.reduced (32, 0));   // la grille garde sa place d'avant (marges de 32) ; seul le bouton va au coin de la vue
        }
        else
        {
            canvas_->setTopAligned (false);
            toolbarRect_ = juce::Rectangle<int> (whole.getX() + 6, whole.getY() + 6, sideW + 12, y - whole.getY());
            canvas_->setBounds (whole.withTrimmedLeft (toolbarRect_.getRight() + 4));
        }
        if (picker_ != nullptr) picker_->setBounds (getLocalBounds());
        if (prompt_ != nullptr) prompt_->setBounds (getLocalBounds());
        if (help_ != nullptr) help_->setBounds (getLocalBounds());
        if (stylePanel_ != nullptr) stylePanel_->setBounds (getLocalBounds());
        if (notchPanel_ != nullptr) notchPanel_->setBounds (getLocalBounds());
        if (scalePanel_ != nullptr) scalePanel_->setBounds (getLocalBounds());
        if (midiPanel_ != nullptr) midiPanel_->setBounds (getLocalBounds());
        return;
    }
    canvas_->setTopAligned (false);
    sideShowButton_.setVisible (false);
    sideHideButton_.setVisible (! editMode_);   // en haut aussi, on peut replier la barre
    auto area = whole.reduced (12, 8);   // barre d'outils dans un conteneur arrondi
    auto layoutRow = [&] (juce::Rectangle<int> row, std::initializer_list<juce::TextButton*> buttons)
    {
        for (auto* b : buttons)
        {
            if (! b->isVisible()) continue;   // seuls les boutons visibles occupent la rangée
            const int w = fit (*b);
            b->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (gap);
        }
    };
    // Rangées qui passent à la ligne quand la place manque.
    auto flow = [&] (std::initializer_list<juce::TextButton*> buttons)
    {
        const auto rect = area;
        int x = 0, y = 0;
        bool any = false;
        for (auto* b : buttons)
        {
            if (! b->isVisible()) continue;
            const int w = fit (*b);
            if (x > 0 && x + w > rect.getWidth()) { x = 0; y += rowH + gap; }
            b->setBounds (rect.getX() + x, rect.getY() + y, w, rowH);
            x += w + gap;
            any = true;
        }
        if (any) area.removeFromTop (y + rowH + gap);
    };
    {
        // rangée des pages : Piste / Globale, puis un onglet par page (à la ligne si la place manque), + Page et Page... en mode Édition
        int x = 0, y = 0;
        auto place = [&] (juce::Component& c, int w)
        {
            if (x > 0 && x + w > area.getWidth()) { x = 0; y += rowH + gap; }
            c.setBounds (area.getX() + x, area.getY() + y, w, rowH);
            x += w + gap;
        };
        if (scopeTrackButton_.isVisible())
        {
            place (scopeTrackButton_, fit (scopeTrackButton_));
            place (scopeGlobalButton_, fit (scopeGlobalButton_));
            x += 16;
        }
        for (auto& tab : pageTabs_) place (*tab, std::max (44, fit (*tab)));
        if (addPageButton_.isVisible()) place (addPageButton_, fit (addPageButton_));
        if (pageMenuButton_.isVisible()) place (pageMenuButton_, fit (pageMenuButton_));
        area.removeFromTop (y + rowH + gap);
    }
    {
        auto top = area.removeFromTop (rowH);
        helpButton_.setBounds (top.removeFromRight (rowH + 8));   // « ? » à droite de la première rangée
        layoutRow (top, { &editButton_, &sideHideButton_, &imageButton_, &fitButton_, &clearImageButton_, &stylesButton_, &saveFacadeButton_, &loadFacadeButton_, &relocateButton_ });
    }
    area.removeFromTop (gap);
    if (editMode_)
    {
        flow ({ &knobButton_, &buttonButton_, &menuButton_, &sliderButton_, &actionButton_, &curveButton_, &labelButton_, &zoneButton_, &keyboardButton_, &lampButton_, &setKeyButton_, &midiButton_, &meterButton_, &gridButton_ });
        flow ({ &linkButton_, &textButton_, &styleButton_, &meterSourceButton_, &gridOptionsButton_, &midiEditButton_, &actionModeButton_, &lampSourceButton_, &orientButton_, &notchesButton_, &scaleButton_, &invertButton_, &captureButton_, &kbChannelButton_, &kbFirstButton_, &kbHoldButton_, &kbWheelsButton_, &kbVelocityButton_, &sameSizeButton_, &layoutButton_, &zoneAlignButton_, &zoneGapButton_, &duplicateButton_, &deleteButton_, &alignButton_ });
    }
    toolbarRect_ = juce::Rectangle<int> (whole.getX(), whole.getY(), whole.getWidth(), area.getY() - whole.getY());
    canvas_->setBounds (whole.withTop (toolbarRect_.getBottom() + 4));
    if (picker_ != nullptr) picker_->setBounds (getLocalBounds());
    if (prompt_ != nullptr) prompt_->setBounds (getLocalBounds());
    if (help_ != nullptr) help_->setBounds (getLocalBounds());
    if (stylePanel_ != nullptr) stylePanel_->setBounds (getLocalBounds());
    if (notchPanel_ != nullptr) notchPanel_->setBounds (getLocalBounds());
    if (scalePanel_ != nullptr) scalePanel_->setBounds (getLocalBounds());
    if (midiPanel_ != nullptr) midiPanel_->setBounds (getLocalBounds());
}

void FacadeView::paint (juce::Graphics& g)
{
    if (toolbarRect_.isEmpty()) return;
    const auto b = toolbarRect_.toFloat().reduced (0.5f);
    g.setColour (colour (Token::PanelBg));
    g.fillRoundedRectangle (b, 10.0f);
    g.setColour (colour (Token::Pb));
    g.drawRoundedRectangle (b, 10.0f, 1.0f);
}

bool FacadeView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("neptunestyle")) return true;
    return false;
}

void FacadeView::filesDropped (const juce::StringArray& files, int, int)
{
    juce::StringArray messages;
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("neptunestyle")) messages.add (StylePanel::owner_message (*this, juce::File (f)));
    if (messages.isEmpty()) return;
    openStyles();
    if (stylePanel_ != nullptr) stylePanel_->setMessage (messages.joinIntoString ("  "));
}

void FacadeView::timerCallback()
{
    if (! isShowing()) return;
    refresh();
    canvas_->tickMeters();
    if (++missingTick_ >= 30) { missingTick_ = 0; refreshMissing(); }   // les fichiers peuvent apparaître ou disparaître : on regarde toutes les deux secondes
    canvas_->syncChildren();
    canvas_->repaint();   // les valeurs peuvent changer (automation, autre vue)
}

void FacadeView::setScopeVisible (bool visible)
{
    scopeTrackButton_.setVisible (visible);
    scopeGlobalButton_.setVisible (visible);
    resized();
}

void FacadeView::setGlobal (bool global)
{
    if (global == global_) return;
    global_ = global;
    scopeTrackButton_.setToggleState (! global_, juce::dontSendNotification);
    scopeGlobalButton_.setToggleState (global_, juce::dontSendNotification);
    revision_ = 0xFFFFFFFFu;   // relecture
    page_ = 0;
    refresh();
}

void FacadeView::refresh()
{
    const int t = global_ ? -1 : processor_.selectedTrack();
    const auto rev = processor_.instrumentRevision();
    if (t == track_ && rev == revision_) return;
    if (canvas_->dragging()) return;
    if (t != track_) { canvas_->releaseKeys(); selection_.clear(); page_ = 0; canvas_->clearPictures(); }
    track_ = t;
    revision_ = rev;
    book_ = global_ ? processor_.globalFacade() : processor_.trackSettings (t).facade;
    page_ = std::clamp (page_, 0, fc::pageCount (book_) - 1);
    facade_ = fc::pageOf (book_, page_);
    rebuildPageTabs();
    refreshMissing();
    updateButtons();
    resized();
}

// Enregistre le livre entier (la Façade de la piste, ou la Façade globale) : un seul geste annulable.
void FacadeView::writeBook (fc::Facade book, const char* undoName)
{
    if (! hasTarget()) return;
    book = fc::sanitised (std::move (book));
    book_ = book;
    page_ = std::clamp (page_, 0, fc::pageCount (book_) - 1);
    facade_ = fc::pageOf (book_, page_);
    if (global_) processor_.uiEditGlobalFacade (book_, undoName, liveMergeKey_);
    else
    {
        auto s = processor_.trackSettings (track_);
        if (s.facade != book_)
        {
            s.facade = book_;
            processor_.uiEditTrack (track_, std::move (s), undoName, liveMergeKey_);
        }
    }
    revision_ = processor_.instrumentRevision();
    rebuildPageTabs();
    updateButtons();
    canvas_->syncChildren();
    canvas_->repaint();
}

void FacadeView::commit (fc::Facade f, const char* undoName)
{
    if (! hasTarget()) return;
    f = fc::sanitised (std::move (f));
    f.pages.clear();
    const bool nameChanged = f.name != fc::pageOf (book_, page_).name;
    writeBook (fc::withPage (book_, page_, f), undoName);
    if (nameChanged) rebuildPageTabs();
}

void FacadeView::setPage (int index)
{
    if (canvas_->dragging()) return;
    index = std::clamp (index, 0, fc::pageCount (book_) - 1);
    if (index == page_ && facade_ == fc::pageOf (book_, page_)) { rebuildPageTabs(); return; }
    canvas_->releaseKeys();
    selection_.clear();
    canvas_->clearPictures();
    page_ = index;
    facade_ = fc::pageOf (book_, page_);
    rebuildPageTabs();
    refreshMissing();
    updateButtons();
    resized();
    canvas_->syncChildren();
    canvas_->repaint();
}

// Onglets des pages : un bouton par page (nom, ou son numéro), la page affichée est allumée.
void FacadeView::rebuildPageTabs()
{
    const int count = fc::pageCount (book_);
    bool same = static_cast<int> (pageTabs_.size()) == count;
    for (int i = 0; same && i < count; ++i)
    {
        const auto name = fc::pageOf (book_, i).name;
        const auto label = name.empty() ? juce::String (i + 1) : juce::String::fromUTF8 (name.c_str());
        same = pageTabs_[static_cast<std::size_t> (i)]->getButtonText() == label;
    }
    if (! same)
    {
        pageTabs_.clear();
        for (int i = 0; i < count; ++i)
        {
            const auto name = fc::pageOf (book_, i).name;
            auto tab = std::make_unique<juce::TextButton> (name.empty() ? juce::String (i + 1) : juce::String::fromUTF8 (name.c_str()));
            tab->setClickingTogglesState (true);
            tab->setRadioGroupId (5872);
            tab->onClick = [this, i] { setPage (i); };
            addAndMakeVisible (*tab);
            pageTabs_.push_back (std::move (tab));
        }
        resized();
    }
    for (int i = 0; i < count; ++i) pageTabs_[static_cast<std::size_t> (i)]->setToggleState (i == page_, juce::dontSendNotification);
    addPageButton_.setEnabled (count < fc::maxPages);
}

void FacadeView::showPageMenu()
{
    const int count = fc::pageCount (book_);
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.page_rename"));
    menu.addItem (2, text ("facade.page_delete"), count > 1);
    menu.addItem (3, text ("facade.page_left"), page_ > 0);
    menu.addItem (4, text ("facade.page_right"), page_ < count - 1);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pageMenuButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice == 0) return;
        if (choice == 1) safe->renamePage();
        else if (choice == 2) { safe->writeBook (fc::removePage (safe->book_, safe->page_), "Facade: delete a page"); safe->setPage (safe->page_); }
        else if (choice == 3) { const int to = safe->page_ - 1; safe->writeBook (fc::movePage (safe->book_, safe->page_, to), "Facade: move a page"); safe->setPage (to); }
        else if (choice == 4) { const int to = safe->page_ + 1; safe->writeBook (fc::movePage (safe->book_, safe->page_, to), "Facade: move a page"); safe->setPage (to); }
    });
}

void FacadeView::renamePage()
{
    if (prompt_ != nullptr || ! hasTarget()) return;
    prompt_ = std::make_unique<TextPrompt> (*this, text ("facade.page_rename"), utf8 (facade_.name),
        [safe = juce::Component::SafePointer<FacadeView> (this)] (const juce::String& value)
        {
            if (safe == nullptr) return;
            safe->writeBook (fc::renamePage (safe->book_, safe->page_, value.toStdString()), "Facade: rename a page");
        });
    addAndMakeVisible (*prompt_);
    prompt_->setBounds (getLocalBounds());
    prompt_->toFront (true);
}

void FacadeView::commitArranged (fc::Facade f, std::set<int> touched, const char* undoName)
{
    fc::arrange (f, touched);
    commit (std::move (f), undoName);
}

void FacadeView::applySingle (const std::function<void (fc::Widget&)>& change, const char* undoName)
{
    auto f = facade_;
    auto* w = f.find (single());
    if (w == nullptr) return;
    change (*w);
    const int id = w->id;
    commitArranged (std::move (f), { id }, undoName);
}

// 0 : canal MIDI, 1 : première note, 2 : vélocité
void FacadeView::openMidiEditor()
{
    const auto* w = facade_.find (single());
    if (midiPanel_ != nullptr || w == nullptr || w->type != fc::WidgetType::Midi) return;
    MidiMessagePanel::Settings initial;
    initial.message = w->midi;
    initial.target = w->midiTarget;
    initial.fader = w->midiFader != 0;
    const int id = w->id;
    midiPanel_ = std::make_unique<MidiMessagePanel> (palette_, tr_, true, initial);
    midiPanel_->onSend = [this] (const std::vector<std::uint8_t>& bytes, int targets) { return processor_.postMidi (bytes, targets); };
    midiPanel_->onApply = [safe = juce::Component::SafePointer<FacadeView> (this), id] (const MidiMessagePanel::Settings& s)
    {
        if (safe == nullptr) return;
        auto f = safe->facade_;
        if (auto* widget = f.find (id))
        {
            widget->midi = s.message;
            widget->midiTarget = s.target;
            widget->midiFader = s.fader ? 1 : 0;
            safe->commit (std::move (f), "Facade: MIDI message");
        }
    };
    midiPanel_->onClose = [safe = juce::Component::SafePointer<FacadeView> (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr) { safe->midiPanel_.reset(); if (safe->canvas_ != nullptr) safe->canvas_->grabKeyboardFocus(); }
        });
    };
    addAndMakeVisible (*midiPanel_);
    midiPanel_->setBounds (getLocalBounds());
    midiPanel_->toFront (true);
}

void FacadeView::showMeterSourceMenu()
{
    const auto* m = facade_.find (single());
    if (m == nullptr || m->type != fc::WidgetType::Meter) return;
    const int current = m->source;
    juce::PopupMenu menu;
    // Un identifiant de menu = la source + 1 (0 est « rien choisi »).
    auto add = [&] (juce::PopupMenu& to, const juce::String& label, int source) { to.addItem (source + 1, label, true, source == current); };
    auto numbered = [&] (const char* key, int count, int base, const std::function<bool (int)>& available)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < count; ++i)
            if (available (i)) add (sub, text (key) + " " + juce::String (i + 1), base + i);
        return sub;
    };
    auto always = [] (int) { return true; };
    auto activeParallel = [this] (int p) { return processor_.parallelActive (p); };
    namespace mx = neptune::core::mixer;
    for (const bool reduction : { false, true })
    {
        if (reduction) { menu.addSeparator(); menu.addSectionHeader (text ("facade.meter_reduction")); }
        add (menu, text (reduction ? "facade.meter_track_reduction" : "facade.meter_track"), reduction ? 2 : 0);
        add (menu, text (reduction ? "facade.meter_master_reduction" : "facade.meter_master"), reduction ? 700 : 1);
        menu.addSubMenu (text ("facade.meter_other_track"), numbered ("facade.meter_track_n", mx::numTracks, reduction ? 400 : 100, always));
        menu.addSubMenu (text ("facade.meter_bus"), numbered ("facade.meter_bus_n", mx::numBuses, reduction ? 500 : 200, always));
        menu.addSubMenu (text ("facade.meter_parallel"), numbered ("facade.meter_parallel_n", mx::numParallels, reduction ? 600 : 300, activeParallel));
    }
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&meterSourceButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        safe->applySingle ([choice] (fc::Widget& w) { w.source = choice - 1; }, "Facade: meter source");
    });
}

void FacadeView::showGridMenu()
{
    const auto* g = facade_.find (single());
    if (g == nullptr || g->type != fc::WidgetType::SceneGrid) return;
    juce::PopupMenu menu, cols, rows, colour, vivid;
    for (int n = 4; n <= 8; ++n)
    {
        cols.addItem (100 + n, juce::String (n), true, g->columns == n);
        rows.addItem (200 + n, juce::String (n), true, g->rows == n);
    }
    for (int i = 0; i < 12; ++i) colour.addItem (300 + i, text (("facade.hue_" + std::to_string (i)).c_str()), true, g->hue == i * 30);
    const int levels[4] = { 15, 40, 65, 90 };
    for (int i = 0; i < 4; ++i) vivid.addItem (400 + i, text (("facade.vivid_" + std::to_string (i)).c_str()), true, g->vivid == levels[i]);
    menu.addSubMenu (text ("facade.grid_cols"), cols);
    menu.addSubMenu (text ("facade.grid_rows"), rows);
    menu.addSubMenu (text ("facade.grid_colour"), colour);
    menu.addSubMenu (text ("facade.grid_vivid"), vivid);
    menu.addItem (600, text ("facade.grid_name_size"));
    menu.addItem (700, text ("facade.grid_note_size"));
    menu.addItem (500, text ("facade.grid_header"), true, g->header != 0);
    menu.addItem (501, text ("facade.grid_set"), g->header != 0, g->setPill != 0);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gridOptionsButton_), [safe = juce::Component::SafePointer<FacadeView> (this), levels] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        if (choice == 600 || choice == 700) { safe->showGridSizePanel (choice == 600); return; }
        safe->applySingle ([choice, levels] (fc::Widget& w)
        {
            if (choice >= 100 && choice < 200) w.columns = choice - 100;
            else if (choice >= 200 && choice < 300) w.rows = choice - 200;
            else if (choice >= 300 && choice < 400) w.hue = (choice - 300) * 30;
            else if (choice >= 400 && choice < 500) w.vivid = levels[choice - 400];
            else if (choice == 501) w.setPill = w.setPill != 0 ? 0 : 1;
            else if (choice == 500) w.header = w.header != 0 ? 0 : 1;
        }, "Facade: scene grid");
    });
}

void FacadeView::showGridSizePanel (bool forName)
{
    const auto* g = facade_.find (single());
    if (g == nullptr || g->type != fc::WidgetType::SceneGrid) return;
    // un petit panneau avec un curseur : la grille se redessine à mesure qu'on le tire
    struct Panel : juce::Component
    {
        Panel (FacadeView& view, bool name, int value) : owner (&view), forName (name)
        {
            title.setText (view.text (name ? "facade.grid_name_size" : "facade.grid_note_size"), juce::dontSendNotification);
            title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
            slider.setRange (50.0, name ? 400.0 : 500.0, 1.0);
            slider.setTextValueSuffix (" %");
            slider.setValue (value, juce::dontSendNotification);
            slider.onValueChange = [this]
            {
                if (owner == nullptr) return;
                const int v = static_cast<int> (std::lround (slider.getValue()));
                owner->liveMergeKey_ = forName ? "grid-name-size" : "grid-note-size";
                owner->applySingle ([v, this] (fc::Widget& w) { (forName ? w.nameSize : w.noteSize) = v; }, "Facade: scene grid text size");
            };
            addAndMakeVisible (title);
            addAndMakeVisible (slider);
            setSize (360, 78);
        }
        ~Panel() override { if (owner != nullptr) owner->liveMergeKey_.clear(); }
        void resized() override { auto r = getLocalBounds().reduced (12, 8); title.setBounds (r.removeFromTop (24)); slider.setBounds (r); }
        juce::Component::SafePointer<FacadeView> owner;
        bool forName;
        juce::Label title;
        juce::Slider slider;
    };
    auto panel = std::make_unique<Panel> (*this, forName, forName ? g->nameSize : g->noteSize);
    juce::CallOutBox::launchAsynchronously (std::move (panel), gridOptionsButton_.getScreenBounds(), nullptr);
}

void FacadeView::editSceneText (int piston)
{
    namespace cb = neptune::core::combi;
    const int bank = processor_.combiStatus().bank;
    const auto cells = processor_.sceneCells (bank, piston + 1);
    if (piston < 0 || piston >= static_cast<int> (cells.size()) || ! cells[static_cast<std::size_t> (piston)].used) return;   // une scène vide n'a pas de nom : on l'enregistre d'abord
    const int key = cb::generalKey (bank, piston);
    auto* w = new juce::AlertWindow (text ("facade.grid_text_title") + " " + juce::String (piston + 1), text ("facade.grid_text_prompt"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", utf8 (cells[static_cast<std::size_t> (piston)].name), text ("facade.grid_name"));
    w->addTextEditor ("note", utf8 (cells[static_cast<std::size_t> (piston)].note), text ("facade.grid_note"));
    w->addTextEditor ("note2", utf8 (cells[static_cast<std::size_t> (piston)].note2), text ("facade.grid_note2"));
    w->addButton (text ("grp.ok"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (text ("mix.cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, key, safe = juce::Component::SafePointer<FacadeView> (this)] (int result)
    {
        if (result != 1 || safe == nullptr) return;
        safe->processor_.uiCombiSetText (key, w->getTextEditorContents ("name").toStdString(), w->getTextEditorContents ("note").toStdString(), w->getTextEditorContents ("note2").toStdString());
    }), true);
}

void FacadeView::showActionModeMenu()
{
    const auto* a = facade_.find (single());
    if (a == nullptr || a->type != fc::WidgetType::Action) return;
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.mode_restore"), true, a->actionMode == 0);
    menu.addItem (2, text ("facade.mode_reset"), true, a->actionMode == 1);
    juce::PopupMenu combinations;
    for (int c = 1; c <= neptune::core::combi::kNumCommands; ++c)
        combinations.addItem (100 + c, combiCommandText ([this] (const char* key) { return text (key); }, c), true, a->actionMode == 1 + c);
    menu.addSubMenu (text ("combi.menu"), combinations);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&actionModeButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        const int mode = choice > 100 ? choice - 100 + 1 : choice - 1;   // 100 + commande : une commande de combinaison (mode 1 + commande)
        safe->applySingle ([mode] (fc::Widget& w) { w.actionMode = mode; }, "Facade: action mode");
    });
}

void FacadeView::showLampSourceMenu()
{
    const auto* l = facade_.find (single());
    if (l == nullptr || l->type != fc::WidgetType::Lamp) return;
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.lamp_param"), true, l->source == 0);
    menu.addItem (2, text ("facade.lamp_channel"), true, l->source == 1);
    menu.addItem (3, text ("facade.lamp_track"), true, l->source == 2);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&lampSourceButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        safe->applySingle ([choice] (fc::Widget& w) { w.source = choice - 1; }, "Facade: lamp source");
    });
}

void FacadeView::showKeyboardMenu (int what)
{
    const auto* k = facade_.find (single());
    if (k == nullptr || (k->type != fc::WidgetType::Keyboard && k->type != fc::WidgetType::Lamp)) return;
    juce::PopupMenu menu;
    juce::TextButton* target = what == 0 ? &kbChannelButton_ : what == 1 ? &kbFirstButton_ : &kbVelocityButton_;
    if (what == 0)
        for (int c = 1; c <= 16; ++c) menu.addItem (c, text ("facade.kb_channel_item") + " " + juce::String (c), true, k->channel == c);
    else if (what == 1)
        for (int note = 12; note <= 96; note += 12) menu.addItem (note + 1, "C" + juce::String (note / 12 - 1) + "  (" + juce::String (note) + ")", true, k->firstNote == note);
    else
        for (const int v : { 32, 64, 80, 100, 127 }) menu.addItem (v + 1, juce::String (v), true, k->velocity == v);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [safe = juce::Component::SafePointer<FacadeView> (this), what] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        safe->applySingle ([what, choice] (fc::Widget& w)
        {
            if (what == 0) w.channel = choice;
            else if (what == 1) w.firstNote = choice - 1;
            else w.velocity = choice - 1;
        }, "Facade: keyboard settings");
    });
}

void FacadeView::applyZone (const std::function<void (fc::Widget&)>& change, const char* undoName)
{
    auto f = facade_;
    auto* z = f.find (single());
    if (z == nullptr || z->type != fc::WidgetType::Zone) return;
    change (*z);
    const int id = z->id;
    commitArranged (std::move (f), { id }, undoName);
}

void FacadeView::addWidget (fc::WidgetType type)
{
    auto f = facade_;
    if (static_cast<int> (f.widgets.size()) >= fc::maxWidgets) return;
    // posé près du centre, décalé à chaque ajout pour ne pas empiler exactement
    const int n = static_cast<int> (f.widgets.size()) % 8;
    auto w = fc::defaultWidget (type, f.freeId(), fc::canvasWidth / 2 - 40 + 24 * n, fc::canvasHeight / 2 - 40 + 24 * n);
    if (type == fc::WidgetType::Label) w.text = "Label";
    if (type == fc::WidgetType::Zone) w.text = "Zone";
    selection_ = { w.id };
    const int newId = w.id;
    f.widgets.push_back (std::move (w));
    commitArranged (std::move (f), { newId }, "Facade: add widget");
}

void FacadeView::addLinked (fc::WidgetType type, neptune::core::ParamId param)
{
    refresh();
    if (! hasTarget()) return;
    auto f = facade_;
    if (static_cast<int> (f.widgets.size()) >= fc::maxWidgets) return;
    const int n = static_cast<int> (f.widgets.size()) % 8;
    auto w = fc::defaultWidget (type, f.freeId(), 60 + 24 * n, 60 + 24 * n);
    w.param = param;
    const int newId = w.id;
    f.widgets.push_back (std::move (w));
    commitArranged (std::move (f), { newId }, "Facade: add widget");
}

void FacadeView::duplicateSelected()
{
    auto f = facade_;
    const auto copies = fc::duplicateWidgets (f, selection_, 24);   // le contenu d'une zone est copié avec elle
    if (copies.empty()) return;
    selection_ = copies;
    commitArranged (std::move (f), copies, "Facade: duplicate widgets");
}

void FacadeView::deleteSelected()
{
    auto f = facade_;
    std::set<int> orphans;   // le contenu d'une zone supprimée est conservé et cherche une autre zone
    for (const auto& w : f.widgets)
        if (selection_.count (w.id) != 0 && w.type == fc::WidgetType::Zone)
            for (const auto& k : f.widgets)
                if (k.parent == w.id && selection_.count (k.id) == 0) orphans.insert (k.id);
    const auto before = f.widgets.size();
    f.widgets.erase (std::remove_if (f.widgets.begin(), f.widgets.end(), [this] (const fc::Widget& w) { return selection_.count (w.id) != 0; }), f.widgets.end());
    if (f.widgets.size() == before) return;
    selection_.clear();
    commitArranged (std::move (f), orphans, "Facade: delete widgets");
}

void FacadeView::showAlignMenu()
{
    juce::PopupMenu grid;
    for (int c = 2; c <= 8; ++c) grid.addItem (100 + c, juce::String (c));
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.align_left"));
    menu.addItem (2, text ("facade.align_right"));
    menu.addItem (3, text ("facade.align_top"));
    menu.addItem (4, text ("facade.align_bottom"));
    menu.addItem (5, text ("facade.align_centre_h"));
    menu.addItem (6, text ("facade.align_centre_v"));
    menu.addSeparator();
    menu.addItem (7, text ("facade.distribute_h"), selection_.size() >= 3);
    menu.addItem (8, text ("facade.distribute_v"), selection_.size() >= 3);
    menu.addSubMenu (text ("facade.grid"), grid);
    juce::PopupMenu circle;
    circle.addItem (21, text ("facade.circle_left"));
    circle.addItem (22, text ("facade.circle_top"));
    circle.addItem (23, text ("facade.circle_right"));
    circle.addItem (24, text ("facade.circle_bottom"));
    menu.addSubMenu (text ("facade.circle"), circle, selection_.size() >= 3);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&alignButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        auto f = safe->facade_;
        const auto ids = safe->selection_;
        if (choice <= 6) fc::alignWidgets (f, ids, static_cast<fc::Align> (choice - 1));
        else if (choice == 7 || choice == 8) fc::distributeWidgets (f, ids, choice == 7);
        else if (choice >= 21 && choice <= 24)
            fc::arrangeCircle (f, ids, ids.count (safe->lastClicked_) != 0 ? safe->lastClicked_ : *ids.begin(), static_cast<fc::CircleStart> (choice - 21));
        else fc::arrangeGrid (f, ids, choice - 100);
        safe->commitArranged (std::move (f), ids, "Facade: arrange widgets");
    });
}

void FacadeView::showZoneLayoutMenu()
{
    const auto* z = facade_.find (single());
    if (z == nullptr || z->type != fc::WidgetType::Zone) return;
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.layout_free"), true, z->layout == 0);
    menu.addItem (2, text ("facade.layout_row"), true, z->layout == 1);
    menu.addItem (3, text ("facade.layout_column"), true, z->layout == 2);
    for (int c = 2; c <= 8; ++c) menu.addItem (10 + c, text ("facade.layout_grid") + " " + juce::String (c), true, z->layout == 3 && z->columns == c);
    menu.addItem (30, text ("facade.circle_left"), true, z->layout == 4 && z->justify == 0);
    menu.addItem (31, text ("facade.circle_top"), true, z->layout == 4 && z->justify == 1);
    menu.addItem (32, text ("facade.circle_right"), true, z->layout == 4 && z->justify == 2);
    menu.addItem (33, text ("facade.circle_bottom"), true, z->layout == 4 && z->justify == 3);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&layoutButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        safe->applyZone ([choice] (fc::Widget& w)
        {
            if (choice <= 3) w.layout = choice - 1;
            else if (choice >= 30) { w.layout = 4; w.justify = choice - 30; }
            else { w.layout = 3; w.columns = choice - 10; }
        }, "Facade: zone layout");
    });
}

void FacadeView::showZoneAlignMenu()
{
    const auto* z = facade_.find (single());
    if (z == nullptr || z->type != fc::WidgetType::Zone) return;
    juce::PopupMenu menu;
    menu.addSectionHeader (text ("facade.align_cross"));
    menu.addItem (1, text ("facade.align_start"), true, z->align == 0);
    menu.addItem (2, text ("facade.align_centre"), true, z->align == 1);
    menu.addItem (3, text ("facade.align_end"), true, z->align == 2);
    menu.addSectionHeader (text ("facade.align_main"));
    menu.addItem (11, text ("facade.justify_start"), true, z->justify == 0);
    menu.addItem (12, text ("facade.justify_centre"), true, z->justify == 1);
    menu.addItem (13, text ("facade.justify_end"), true, z->justify == 2);
    menu.addItem (14, text ("facade.justify_spread"), true, z->justify == 3);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&zoneAlignButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        safe->applyZone ([choice] (fc::Widget& w)
        {
            if (choice <= 3) w.align = choice - 1;
            else w.justify = choice - 11;
        }, "Facade: zone alignment");
    });
}

void FacadeView::showZoneGapMenu()
{
    const auto* z = facade_.find (single());
    if (z == nullptr || z->type != fc::WidgetType::Zone) return;
    juce::PopupMenu menu;
    for (const int g : { 0, 4, 8, 12, 16, 24, 32, 48 }) menu.addItem (100 + g, juce::String (g) + " px", true, z->gap == g);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&zoneGapButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice < 100) return;
        safe->applyZone ([choice] (fc::Widget& w) { w.gap = choice - 100; }, "Facade: zone spacing");
    });
}

void FacadeView::chooseImage()
{
    chooser_ = std::make_unique<juce::FileChooser> (text ("facade.choose_image"), juce::File::getSpecialLocation (juce::File::userPicturesDirectory), "*.png;*.jpg;*.jpeg;*.svg");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [safe = juce::Component::SafePointer<FacadeView> (this)] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr) return;
                               const auto file = c.getResult();
                               if (! file.existsAsFile()) return;
                               auto f = safe->facade_;
                               f.image = file.getFullPathName().toStdString();
                               safe->commit (std::move (f), "Facade: background");
                           });
}

void FacadeView::showFitMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.fit_fit"), true, facade_.fit == fc::ImageFit::Fit);
    menu.addItem (2, text ("facade.fit_stretch"), true, facade_.fit == fc::ImageFit::Stretch);
    menu.addItem (3, text ("facade.fit_fill"), true, facade_.fit == fc::ImageFit::Fill);
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fitButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        auto f = safe->facade_;
        f.fit = static_cast<fc::ImageFit> (choice - 1);
        safe->commit (std::move (f), "Facade: background fit");
    });
}

void FacadeView::openPicker (bool capture)
{
    const auto* current = facade_.find (single());
    if (picker_ != nullptr || current == nullptr) return;
    const int id = current->id;
    std::set<neptune::core::ParamId> initial;
    if (capture)
        for (const auto& e : current->snapshot) initial.insert (e.first);
    picker_ = std::make_unique<Picker> (*this, track_, capture, std::move (initial),
        [safe = juce::Component::SafePointer<FacadeView> (this), id] (neptune::core::ParamId param)
        {
            if (safe == nullptr) return;
            auto f = safe->facade_;
            if (auto* w = f.find (id)) { w->param = param; safe->commit (std::move (f), "Facade: link parameter"); }
        },
        [safe = juce::Component::SafePointer<FacadeView> (this), id] (const std::set<neptune::core::ParamId>& ids)
        {
            if (safe == nullptr) return;
            auto f = safe->facade_;
            if (auto* w = f.find (id))
            {
                w->snapshot.clear();
                for (const auto pid : ids)
                    if (safe->processor_.parameters().find (pid) != nullptr) w->snapshot.emplace_back (pid, safe->processor_.parameters().get (pid));
                safe->commit (std::move (f), "Facade: capture values");
            }
        });
    addAndMakeVisible (*picker_);
    picker_->setBounds (getLocalBounds());
    picker_->toFront (true);
}

void FacadeView::editText()
{
    const auto* current = facade_.find (single());
    if (current == nullptr || prompt_ != nullptr) return;
    const int id = current->id;
    prompt_ = std::make_unique<TextPrompt> (*this, text ("facade.text_title"), utf8 (current->text),
        [safe = juce::Component::SafePointer<FacadeView> (this), id] (const juce::String& value)
        {
            if (safe == nullptr) return;
            auto f = safe->facade_;
            if (auto* widget = f.find (id)) { widget->text = value.toStdString(); safe->commit (std::move (f), "Facade: widget text"); }
        });
    addAndMakeVisible (*prompt_);
    prompt_->setBounds (getLocalBounds());
    prompt_->toFront (true);
}

void FacadeView::openNotches()
{
    const auto* w = facade_.find (single());
    if (notchPanel_ != nullptr || w == nullptr || (w->type != fc::WidgetType::Knob && w->type != fc::WidgetType::Slider)) return;
    notchPanel_ = std::make_unique<NotchPanel> (*this, w->id);
    addAndMakeVisible (*notchPanel_);
    notchPanel_->setBounds (getLocalBounds());
    notchPanel_->toFront (true);
}

void FacadeView::openScale()
{
    const auto* w = facade_.find (single());
    if (scalePanel_ != nullptr || w == nullptr || (w->type != fc::WidgetType::Knob && w->type != fc::WidgetType::Slider)) return;
    scalePanel_ = std::make_unique<ScalePanel> (*this, w->id);
    addAndMakeVisible (*scalePanel_);
    scalePanel_->setBounds (getLocalBounds());
    scalePanel_->toFront (true);
}

void FacadeView::openStyles()
{
    if (stylePanel_ != nullptr) return;
    stylePanel_ = std::make_unique<StylePanel> (*this);
    addAndMakeVisible (*stylePanel_);
    stylePanel_->setBounds (getLocalBounds());
    stylePanel_->toFront (true);
}

// Affecte un style à la sélection (les widgets qui en prennent un) ; « Gérer les styles... » ouvre la bibliothèque.
void FacadeView::showStyleMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, text ("facade.style_none"));
    for (const auto& st : facade_.styles) menu.addItem (100 + st.id, utf8 (st.name));
    menu.addSeparator();
    menu.addItem (2, text ("facade.styles"));
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&styleButton_), [safe = juce::Component::SafePointer<FacadeView> (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0) return;
        if (choice == 2) { safe->openStyles(); return; }
        auto f = safe->facade_;
        for (auto& w : f.widgets)
            if (safe->selection_.count (w.id) != 0 && takesStyle (w.type)) w.style = choice == 1 ? 0 : choice - 100;
        safe->commit (std::move (f), "Facade: widget style");
    });
}

void FacadeView::openNotice (const juce::String& title, const juce::String& body)
{
    help_.reset();
    help_ = std::make_unique<HelpPanel> (*this, title, body);
    addAndMakeVisible (*help_);
    help_->setBounds (getLocalBounds());
    help_->toFront (true);
    help_->grabKeyboardFocus();
}

void FacadeView::saveFacadeFile()
{
    if (! hasTarget()) return;
    chooser_ = std::make_unique<juce::FileChooser> (text ("facade.save_title"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Facade.neptunefacade"), "*.neptunefacade");
    chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                           [safe = juce::Component::SafePointer<FacadeView> (this), facade = book_, track = track_] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr || c.getResult() == juce::File()) return;
                               auto target = c.getResult();
                               if (! target.hasFileExtension ("neptunefacade")) target = target.withFileExtension ("neptunefacade");
                               const auto report = facade_bundle::save (facade, track, target);
                               const auto message = report.ok
                                   ? juce::String::fromUTF8 (safe->tr_.format ("facade.saved", { target.getFullPathName().toStdString(), std::to_string (report.images), std::to_string (report.packs), std::to_string (report.missing) }).c_str())
                                   : juce::String::fromUTF8 (safe->tr_.format ("facade.save_failed", { target.getFullPathName().toStdString() }).c_str());
                               safe->openNotice (safe->text ("facade.save_title"), message);
                           });
}

void FacadeView::loadFacadeFile()
{
    if (! hasTarget()) return;
    chooser_ = std::make_unique<juce::FileChooser> (text ("facade.load_title"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.neptunefacade");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [safe = juce::Component::SafePointer<FacadeView> (this)] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr || ! c.getResult().existsAsFile()) return;
                               fc::Facade loaded;
                               const auto assets = stylelib::directory().getParentDirectory().getChildFile ("facade-assets");
                               const auto report = facade_bundle::load (c.getResult(), safe->track_, assets, loaded);
                               if (! report.ok)
                               {
                                   safe->openNotice (safe->text ("facade.load_title"), juce::String::fromUTF8 (safe->tr_.format ("facade.load_failed", { report.error.toStdString() }).c_str()));
                                   return;
                               }
                               const int widgets = static_cast<int> (loaded.widgets.size());
                               safe->selection_.clear();
                               safe->canvas_->clearPictures();
                               safe->page_ = 0;
                               safe->writeBook (std::move (loaded), "Facade: load a Facade file");
                               safe->setPage (0);
                               safe->openNotice (safe->text ("facade.load_title"), juce::String::fromUTF8 (safe->tr_.format ("facade.loaded", { std::to_string (widgets), std::to_string (report.images), std::to_string (report.packsInstalled) }).c_str()));
                           });
}

// Images introuvables (fond, styles) : compte, et affiche le bouton « Relocaliser... ».
void FacadeView::refreshMissing()
{
    int missing = 0;
    auto copy = book_;   // toutes les pages
    fc::forEachImagePath (copy, [&missing] (std::string& path) { if (! stylelib::exists (path)) ++missing; });
    if (missing != missing_) { missing_ = missing; updateButtons(); }
}

// Un fichier d'image déplacé : on désigne le nouvel emplacement de la première image manquante ; si le nom est le même, tous les chemins
// qui partageaient son ancien dossier suivent (pour peu que le fichier existe au nouvel endroit).
void FacadeView::relocate()
{
    std::string firstMissing;
    auto copy = book_;
    fc::forEachImagePath (copy, [&firstMissing] (std::string& path) { if (firstMissing.empty() && ! stylelib::exists (path)) firstMissing = path; });
    if (firstMissing.empty()) return;
    const juce::File oldFile (juce::String::fromUTF8 (firstMissing.c_str()));
    chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 (tr_.format ("facade.relocate_choose", { oldFile.getFileName().toStdString() }).c_str()),
                                                    juce::File::getSpecialLocation (juce::File::userPicturesDirectory), "*.png;*.jpg;*.jpeg;*.svg");
    chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [safe = juce::Component::SafePointer<FacadeView> (this), firstMissing, oldFile] (const juce::FileChooser& c)
                           {
                               if (safe == nullptr || ! c.getResult().existsAsFile()) return;
                               const auto chosen = c.getResult();
                               const bool sameName = chosen.getFileName() == oldFile.getFileName();
                               const auto oldDir = oldFile.getParentDirectory().getFullPathName() + juce::File::getSeparatorString();
                               const auto newDir = chosen.getParentDirectory();
                               auto f = safe->book_;
                               fc::forEachImagePath (f, [&] (std::string& path)
                               {
                                   if (stylelib::exists (path)) return;
                                   if (path == firstMissing) { path = chosen.getFullPathName().toStdString(); return; }
                                   const auto p = juce::String::fromUTF8 (path.c_str());
                                   if (sameName && p.startsWith (oldDir))
                                   {
                                       const auto candidate = newDir.getChildFile (p.substring (oldDir.length()));
                                       if (candidate.existsAsFile()) path = candidate.getFullPathName().toStdString();
                                   }
                               });
                               safe->canvas_->clearPictures();
                               safe->writeBook (std::move (f), "Facade: relocate images");
                               safe->refreshMissing();
                           });
}

void FacadeView::openHelp()
{
    if (help_ != nullptr) return;
    help_ = std::make_unique<HelpPanel> (*this, text ("facade.help_title"), text ("facade.help_body"));
    addAndMakeVisible (*help_);
    help_->setBounds (getLocalBounds());
    help_->toFront (true);
    help_->grabKeyboardFocus();
}
