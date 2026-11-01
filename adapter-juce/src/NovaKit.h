#pragma once
// Composants communs du style NOVA (maquettes design/nova/ui/*.html), réutilisables par tous les plugins de la série.
//
// Principe d'échelle : aucun composant n'utilise de transformation (AffineTransform). Chaque dimension est écrite en pixels de la
// maquette (gabarit 1160 x 780) et passée par Skin::px(), qui la multiplie par le facteur d'échelle courant ; polices et traits
// suivent. Quand le facteur change, l'éditeur appelle Skin::setScale() puis refait la mise en page : les images mises en cache
// sont recréées à la vraie taille (pas de rééchantillonnage, voir la leçon de La Flèche dans docs/passage-de-relais.md).
//
// Les composants liés à un paramètre implémentent Refreshable : l'éditeur appelle refresh() à 30 Hz, et chaque composant ne se
// redessine que si la version du paramètre a changé.
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "NeptuneKnob.h"
#include "ParamHost.h"
#include "neptune/core/Theme.h"

namespace nova
{
// ------------------------------------------------------------------------------------------------ habillage + échelle
class Skin
{
public:
    Skin (const neptune::core::theme::Palette&, neptune::core::theme::KnobStyle,
          std::optional<neptune::core::theme::ThemeId> id = std::nullopt);

    const neptune::core::theme::Palette& palette() const noexcept { return palette_; }
    neptune::core::theme::KnobStyle knobStyle() const noexcept { return knob_; }
    const neptune::core::theme::Metrics& metrics() const noexcept { return metrics_; }
    neptune::core::theme::ControlStyle controlStyle() const noexcept { return control_; }
    bool highContrast() const noexcept { return control_ == neptune::core::theme::ControlStyle::HighContrast; }

    // Agrandissement du texte des pastilles dont la taille n'est pas fixée à la main (1 : taille d'origine). Un texte trop long pour sa pastille est réduit.
    float pillTextScale() const noexcept { return pillTextScale_; }
    void setPillTextScale (float s) noexcept { pillTextScale_ = s; }
    float scale() const noexcept { return scale_; }
    void setScale (float s) noexcept { scale_ = juce::jlimit (0.3f, 4.0f, s); }
    float px (float v) const noexcept { return v * scale_; }
    int ipx (float v) const noexcept { return juce::roundToInt (v * scale_); }

    juce::Colour colour (neptune::core::theme::Token, float alpha = 1.0f) const;
    // Remplit `r` avec le jeton (couleur unie ou dégradé vertical du thème)
    void fill (juce::Graphics&, neptune::core::theme::Token, juce::Rectangle<float> r, float radius = 0.0f) const;
    juce::FontOptions font (float designSize, bool bold = false) const;
    float borderWidth() const noexcept { return px (static_cast<float> (metrics_.borderWidth)); }

    // Dégradé « accent -> rose » des états actifs de la maquette (rgba accent .18 -> rose .10 ; opaque en haut contraste)
    void fillActive (juce::Graphics&, juce::Rectangle<float> r, float radius, float strength = 1.0f) const;

private:
    neptune::core::theme::Palette palette_;
    neptune::core::theme::KnobStyle knob_;
    neptune::core::theme::Metrics metrics_;
    neptune::core::theme::ControlStyle control_;
    float scale_ = 1.0f;
    float pillTextScale_ = 1.0f;
};

// Facteur d'échelle pour une fenêtre w x h et un gabarit dw x dh (proportions conservées).
float fitScale (int w, int h, int dw, int dh, float minScale, float maxScale);
// Rectangle du gabarit mis à l'échelle, centré dans la fenêtre.
juce::Rectangle<int> fitRect (int w, int h, int dw, int dh, float scale);

// Aspect de la fenêtre qui reste constant (proportions du gabarit) : à appeler depuis un ComponentBoundsConstrainer.
void configureConstrainer (juce::ComponentBoundsConstrainer&, int dw, int dh, float minScale, float maxScale);
// Fenêtre étirable en largeur : la hauteur fixe l'échelle (proportions du gabarit conservées en hauteur), la largeur en plus n'agrandit que la zone de droite.
void configureStretchConstrainer (juce::ComponentBoundsConstrainer&, int dw, int dh, float minScale, float maxScale, int maxWidth);

// ------------------------------------------------------------------------------------------------ éléments sans état
struct Refreshable
{
    virtual ~Refreshable() = default;
    virtual void refresh() = 0;
};

// Gestes d'édition complets sur un paramètre
void setParam (ParamHost&, neptune::core::ParamId, double plain);
void setParamNormalised (ParamHost&, neptune::core::ParamId, double normalised);

// Pastille de numéro / légende d'en-tête : fond de l'en-tête, logo, nom et sous-titre.
// Full : logo, nom, sous-titre et version (plugin séparé). Window : idem sans la version à droite (la fenêtre y met ses boutons ; la version suit alors `sub`).
// Compact : seulement le nom de l'instrument, à gauche (éditeur intégré dans une fenêtre qui a déjà son propre en-tête).
enum class HeaderStyle { Full, Window, Compact };
void paintHeaderBar (juce::Graphics&, const Skin&, juce::Rectangle<float>, const juce::String& word, const juce::String& sub, HeaderStyle = HeaderStyle::Full);
void paintBottomBar (juce::Graphics&, const Skin&, juce::Rectangle<float>);

// ------------------------------------------------------------------------------------------------ cadre-carte (.panel)
class Panel : public juce::Component
{
public:
    explicit Panel (const Skin&, neptune::core::theme::Token fill = neptune::core::theme::Token::PanelBg);
    void setTitle (const juce::String&);
    void setMeta (const juce::String&);   // texte atténué à droite du titre
    void setMetaRightInset (float designPx) { metaInset_ = designPx; repaint(); }   // laisse de la place à un bouton en haut à droite
    juce::Rectangle<int> contentArea() const;   // zone sous le titre
    void paint (juce::Graphics&) override;

private:
    const Skin& skin_;
    neptune::core::theme::Token fill_;
    juce::String title_, meta_;
    float metaInset_ = 0.0f;
};

// ------------------------------------------------------------------------------------------------ pastille bouton (.pill-btn / .peak-btn / .icon-btn)
class Pill : public juce::Component, public Refreshable
{
public:
    enum class Look { Pill, Small, Icon };
    Pill (const Skin&, Look = Look::Pill);

    void setText (const juce::String&);
    void setOn (bool);
    bool isOn() const noexcept { return on_; }
    void setStatic (bool s) { static_ = s; repaint(); }    // pastille d'information, non cliquable
    void setDim (bool d) { dim_ = d; repaint(); }
    void setIcon (juce::Path, bool strokeOnly = true);     // chemin dans un carré 24 x 24
    void setDot (bool shown) { dot_ = shown; repaint(); }
    void setTintToken (neptune::core::theme::Token t) { tint_ = t; repaint(); }
    // Largeur (maquette) qui entoure le texte actuel sans le couper
    float idealWidth() const;
    void setFontSize (float designSize) { fontSize_ = designSize; repaint(); }   // taille du texte (maquette) ; 0 : celle du type de pastille  // couleur de l'état actif (défaut : Accent)

    // Liaison à un paramètre : bascule (booléen) ou choix cyclique parmi `choices` (libellés à afficher après `label`).
    void bindToggle (ParamHost&, neptune::core::ParamId, juce::String label);
    void bindChoice (ParamHost&, neptune::core::ParamId, juce::String label, std::vector<juce::String> choices);
    // Réglage continu au glisser : un glissement vertical règle le paramètre (160 px = toute la plage, Maj = 10 fois plus fin) ; le double-clic appelle onClick
    // (en général la modale de saisie). Le texte est « libellé  valeur » (ou la valeur seule si le libellé est vide).
    void bindValue (ParamHost&, neptune::core::ParamId, juce::String label);
    void refresh() override;
    std::function<void()> onClick;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover_ = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    const Skin& skin_;
    Look look_;
    juce::String text_;
    juce::Path icon_;
    bool iconStroke_ = true, on_ = false, static_ = false, dim_ = false, dot_ = false, hover_ = false;
    neptune::core::theme::Token tint_ = neptune::core::theme::Token::Accent;
    ParamHost* host_ = nullptr;
    neptune::core::ParamId id_ = 0;
    juce::String label_;
    std::vector<juce::String> choices_;
    std::uint32_t seen_ = 0xFFFFFFFFu;
    bool valueMode_ = false, dragging_ = false;
    double startNorm_ = 0.0;
    float fontSize_ = 0.0f;
};

// ------------------------------------------------------------------------------------------------ texte
class Text : public juce::Component
{
public:
    Text (const Skin&, neptune::core::theme::Token colour = neptune::core::theme::Token::TextDim, float designSize = 11.0f, bool bold = false);
    void setText (const juce::String&);
    void paint (juce::Graphics&) override;

private:
    const Skin& skin_;
    neptune::core::theme::Token colour_;
    float size_;
    bool bold_;
    juce::String text_;
};

// ------------------------------------------------------------------------------------------------ interrupteur (.switch)
class Switch : public juce::Component, public Refreshable
{
public:
    explicit Switch (const Skin&);
    void setOn (bool);
    bool isOn() const noexcept { return on_; }
    void bind (ParamHost&, neptune::core::ParamId);
    void refresh() override;
    std::function<void (bool)> onChange;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    const Skin& skin_;
    bool on_ = false;
    ParamHost* host_ = nullptr;
    neptune::core::ParamId id_ = 0;
    std::uint32_t seen_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ entrée du menu latéral (.nav-item)
class NavItem : public juce::Component
{
public:
    NavItem (const Skin&, juce::String label, juce::Path icon);
    void setActive (bool);
    std::function<void()> onClick;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    const Skin& skin_;
    juce::String label_;
    juce::Path icon_;
    bool active_ = false;
};

// ------------------------------------------------------------------------------------------------ carte de potard (.knob-card)
class KnobCard : public juce::Component, public Refreshable
{
public:
    // `pink` : variante à titre rose ; `bipolar` n'est pas géré ici (le dessin du potard le sera avec les modules concernés)
    KnobCard (const Skin&, ParamHost&, neptune::core::ParamId, juce::String title, NeptuneKnob::OpenModal, bool pink = false);

    void setTitle (const juce::String& t) { title_ = t; repaint(); }
    // Course du potard limitée à [lo, hi] (valeurs du paramètre) : au-delà, la modale (appui long) reste libre sur toute la plage du paramètre.
    void setDisplayRange (double lo, double hi, double exponent = 1.0);
    void setAutoRange (bool a) { autoRange_ = a; }   // les bornes affichées suivent le paramètre lié
    void setParam (neptune::core::ParamId, juce::String title);   // rebranche le potard sur un autre paramètre
    void setRangeLabels (juce::String lo, juce::String hi) { lo_ = std::move (lo); hi_ = std::move (hi); resized(); repaint(); }
    void setPlain (bool plain) { plain_ = plain; repaint(); }   // sans fond de carte (zones groupées)
    void setCompact (bool c) { compact_ = c; resized(); repaint(); }   // potard de 48 px, textes réduits (zones étroites)
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    const Skin& skin_;
    ParamHost& host_;
    neptune::core::ParamId id_;
    juce::String title_, lo_, hi_, valueText_;
    bool pink_, plain_ = false, compact_ = false, autoRange_ = false;
    std::uint32_t seen_ = 0xFFFFFFFFu;
    std::unique_ptr<NeptuneKnob> knob_;
};

// ------------------------------------------------------------------------------------------------ petit curseur horizontal (.sub-control)
class SubSlider : public juce::Component, public Refreshable
{
public:
    SubSlider (const Skin&, ParamHost&, neptune::core::ParamId, juce::String title, bool pink = false);
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> trackArea() const;
    void applyFromX (float x);
    const Skin& skin_;
    ParamHost& host_;
    neptune::core::ParamId id_;
    juce::String title_;
    bool pink_, dragging_ = false;
    std::uint32_t seen_ = 0xFFFFFFFFu;
};

// ------------------------------------------------------------------------------------------------ vu-mètre vertical à deux barres (.meter-group)
class VMeter : public juce::Component
{
public:
    enum class Kind { Level, Reduction };   // Level : monte depuis le bas (dB, plage floorDb..0) ; Reduction : descend depuis le haut
    VMeter (const Skin&, juce::String title, Kind = Kind::Level, bool pink = false);

    void setRange (float floorDb, float ceilingDb) { floor_ = floorDb; ceil_ = ceilingDb; }
    void setDecayDbPerSecond (float d) { decay_ = d; }   // retombée linéaire (dB par seconde) ; la crête reste affichée jusqu'à la remise à zéro
    // Niveaux instantanés en dB (Level) ou réduction positive en dB (Reduction) ; retombée, crête et valeur gérées ici.
    // Retourne true si l'affichage a changé (donc un repaint a été demandé).
    bool update (float leftDb, float rightDb);
    void setModeLabel (const juce::String& m) { mode_ = m; repaint(); }
    void setModeVisible (bool v) { showMode_ = v; repaint(); }
    std::function<void()> onModeClick;   // clic sur le bouton de mode (PEAK / RMS ...)
    void resetPeak() { peak_[0] = peak_[1] = -120.0f; repaint(); }
    void setZones (bool z) { zones_ = z; repaint(); }   // Level : vert jusqu'à -18 dB, jaune jusqu'à -6 dB, rouge au-delà (au lieu du dégradé du thème)

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> barsArea() const;
    juce::Rectangle<float> modeArea() const;
    const Skin& skin_;
    juce::String title_, mode_ = "PEAK";
    Kind kind_;
    bool pink_, showMode_ = true, zones_ = false;
    float floor_ = -60.0f, ceil_ = 0.0f;
    float decay_ = 24.0f;
    double lastTime_ = 0.0;
    float shown_[2] = { -120.0f, -120.0f }, peak_[2] = { -120.0f, -120.0f }, painted_[4] = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };
};

// ------------------------------------------------------------------------------------------------ bouton d'information « ? » (bulle d'aide au clic)
class InfoButton : public juce::Component
{
public:
    InfoButton (const Skin&, juce::String title, juce::String body);
    void setTexts (juce::String title, juce::String body) { title_ = std::move (title); body_ = std::move (body); }   // le potard rebranché change de sujet
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover_ = false; repaint(); }
    void mouseUp (const juce::MouseEvent&) override;

private:
    const Skin& skin_;
    juce::String title_, body_;
    bool hover_ = false;
};

// ------------------------------------------------------------------------------------------------ fenêtre de réglages (bulle) : une pastille par réglage
struct SettingItem
{
    neptune::core::ParamId id;
    juce::String label;
    std::vector<juce::String> choices;   // vide : bascule
};
// Ouvre une bulle ancrée sur `anchor` avec les réglages, liés à leurs paramètres ; deux colonnes. Elle garde sa propre copie de l'habillage.
void showSettingsPopup (juce::Component* anchor, const Skin&, ParamHost&, const juce::String& title, std::vector<SettingItem> items);

// ------------------------------------------------------------------------------------------------ LookAndFeel (menus, info-bulles, barres de défilement)
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit LookAndFeel (const Skin&);
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    juce::Font getPopupMenuFont() override;
    // Agrandissement du texte (et donc des lignes) des menus ouverts avec ce style (1 : taille d'origine)
    void setPopupScale (float s) noexcept { popupScale_ = s; }
    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                        int thumbSize, bool isMouseOver, bool isMouseDown) override;
    // Commandes JUCE d'origine posées dans un éditeur NOVA (matrice, panneaux de vélocité, de polyphonie, de filtres...) : même habillage que les pastilles.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    // Agrandissement du texte des boutons JUCE (1 : taille d'origine). Au-delà de 1, la taille reste proportionnée à la hauteur du bouton.
    void setButtonTextScale (float s) noexcept { buttonTextScale_ = s; }
    float buttonFontSize (int buttonHeight) const noexcept;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

private:
    const Skin& skin_;
    float buttonTextScale_ = 1.0f, popupScale_ = 1.0f;
};
} // namespace nova
