#pragma once
// La Façade (spec §15) : interface personnalisée d'une piste. Une image de fond et des widgets posés librement, chacun lié à un
// paramètre existant (couche Paramètre générique) ; aucun nouveau moteur, seulement une vue alternative. La Façade fait partie des
// réglages de bande de la piste (StripSettings) : sauvegardée dans le projet, annulable comme les autres réglages de bande.
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "neptune/core/MidiSend.h"
#include "neptune/core/Parameter.h"
#include "neptune/core/VelocityResponse.h"

namespace neptune::core::facade
{
// Taille logique du canevas : la vue le met à l'échelle de la place disponible.
constexpr int canvasWidth = 1000;
constexpr int canvasHeight = 600;
constexpr int maxWidgets = 200;
constexpr int minWidgetSize = 16;
constexpr int keyboardWheelsWidth = 60;   // largeur (dans le canevas) de la bande des molettes d'un clavier qui en a
constexpr std::size_t maxTextLength = 64;
constexpr std::size_t maxImagePath = 1024;

// Menu : liste de valeurs d'un paramètre à choix ou à pas. Action : instantané de valeurs (plusieurs paramètres restaurés d'un coup).
// Curve : courbe de transfert de vélocité de la piste (Sampler).
// Zone : conteneur (cadre avec titre) qui possède les widgets posés dedans et, au choix, les range en ligne, en colonne ou en matrice.
// Keyboard : clavier virtuel lié à un canal MIDI de la piste (un manuel d'orgue = un clavier) ; allume les notes jouées sur ce canal.
// Slider : curseur (tirant) horizontal ou vertical lié à un paramètre ; crans automatiques sur un paramètre à pas.
// Meter : vumètre stéréo (crête et maintien de crête) ou réduction de gain d'un compresseur, vertical ou horizontal. Source : voir MeterSource.
// Midi : envoie un message MIDI (bouton : à chaque clic ; ou curseur : la valeur suit le geste), vers Neptune comme un clavier et/ou vers l'hôte.
// Set : touche « Set » d'une console : une fois armée, le prochain piston (bouton Action) cliqué mémorise les valeurs courantes de ses paramètres.
// Lamp : voyant allumé selon un paramètre (source 0), les notes d'un canal (1) ou les notes de la piste (2).
enum class WidgetType { Knob = 0, Button = 1, Label = 2, Menu = 3, Action = 4, Curve = 5, Zone = 6, Keyboard = 7, Slider = 8, Lamp = 9, Set = 10, Midi = 11, Meter = 12, SceneGrid = 13 };
constexpr int numWidgetTypes = 14;
constexpr int maxStyles = 64;
constexpr int maxStripFrames = 512;
constexpr std::size_t maxNotches = 128;

// Style d'images (PNG, JPG ou SVG), réutilisable par plusieurs widgets. Chaque widget n'utilise que ce qui le concerne ; une image absente
// laisse le dessin d'origine.
//  - Bouton, Action, Voyant : `off` et `on` ; en plus `offHover` / `onHover` (souris dessus) et `disabled` (widget non lié).
//  - Curseur : `track` (le rail) et `handle` (la poignée).
//  - Potard : une bande `strip` de `frames` images (empilées verticalement, ou côte à côte si `stripHorizontal`), une par position ;
//    ou deux couches `body` (fixe) et `pointer` (tournée par le code entre `angleMin` et `angleMax` degrés autour de `pivotX`, `pivotY`).
//  - Clavier : `keyWhite`, `keyWhiteOn`, `keyBlack`, `keyBlackOn` (touche au repos / enfoncée ou jouée).
//  - Zone : `frame`, découpé en neuf zones par `slice*` (pourcentages de l'image) pour s'étirer sans déformer les coins.
//  - Texte posé sur l'image (boutons, actions, curseurs, potards, voyants, zones) : couleur, taille, place, gras.
struct ImageStyle
{
    int id = 0;
    std::string name;
    std::string off, on, offHover, onHover, disabled;
    std::string track, handle;
    std::string strip, body, pointer;
    std::string keyWhite, keyWhiteOn, keyBlack, keyBlackOn;
    std::string frame;
    int frames = 0;
    bool stripHorizontal = false;
    int angleMin = -135, angleMax = 135;   // degrés, 0 = l'aiguille dessinée vers le haut
    int pivotX = 50, pivotY = 50;          // centre de rotation de `pointer`, en % de l'image
    int sliceLeft = 25, sliceTop = 25, sliceRight = 25, sliceBottom = 25;   // % de la largeur / hauteur de `frame`
    int sliceScale = 100;                  // % : taille d'affichage des coins par rapport à la taille naturelle du dessin
    std::string textColour;                // "#rrggbb" ; vide : couleur du thème
    int textSize = 0;                      // % de la hauteur du widget ; 0 : automatique
    int textPosition = 0;                  // 0 centre, 1 haut, 2 bas, 3 gauche, 4 droite
    bool textBold = true;
    std::string keyLit;                    // "#rrggbb" : teinte d'une touche allumée quand le style n'a pas d'image pour elle ; vide : accent du thème

    bool operator== (const ImageStyle& o) const noexcept
    {
        return id == o.id && name == o.name && off == o.off && on == o.on && offHover == o.offHover && onHover == o.onHover && disabled == o.disabled
            && track == o.track && handle == o.handle && strip == o.strip && body == o.body && pointer == o.pointer
            && keyWhite == o.keyWhite && keyWhiteOn == o.keyWhiteOn && keyBlack == o.keyBlack && keyBlackOn == o.keyBlackOn && frame == o.frame
            && frames == o.frames && stripHorizontal == o.stripHorizontal && angleMin == o.angleMin && angleMax == o.angleMax
            && pivotX == o.pivotX && pivotY == o.pivotY && sliceLeft == o.sliceLeft && sliceTop == o.sliceTop && sliceRight == o.sliceRight
            && sliceBottom == o.sliceBottom && sliceScale == o.sliceScale && textColour == o.textColour && textSize == o.textSize
            && textPosition == o.textPosition && textBold == o.textBold && keyLit == o.keyLit;
    }
};

// Les champs d'image d'un style, avec leur nom canonique (celui du fichier dans un pack et de la clé JSON).
struct ImageRole
{
    const char* name;
    std::string ImageStyle::* field;
};
const std::vector<ImageRole>& imageRoles();

constexpr int zonePad = 10;          // marge intérieure d'une zone
constexpr int zoneTitleHeight = 24;  // bandeau du titre (quand la zone a un titre)

// Rangement du contenu d'une zone. `align` : sur l'axe transversal (ligne : haut / centre / bas ; colonne : gauche / centre / droite ;
// matrice : dans la cellule, sur les deux axes). `justify` : sur l'axe principal (début / centre / fin / répartition égale).
// Cercle : le contenu est réparti à angles égaux sur un cercle, dans le sens horaire ; `justify` donne la place du premier
// (0 à gauche, 1 en haut, 2 à droite, 3 en bas).
enum class ZoneLayout { Free = 0, Row = 1, Column = 2, Grid = 3, Circle = 4 };
constexpr int numZoneLayouts = 5;
enum class CircleStart { Left = 0, Top = 1, Right = 2, Bottom = 3 };
enum class CrossAlign { Start = 0, Centre = 1, End = 2 };
enum class Justify { Start = 0, Centre = 1, End = 2, Spread = 3 };
constexpr int maxZoneGap = 64;
constexpr int maxZoneColumns = 12;
constexpr std::size_t maxSnapshot = 64;

// Ajustement de l'image de fond au canevas.
enum class ImageFit { Fit = 0, Stretch = 1, Fill = 2 }; // ajuster (bandes), étirer, remplir (rogner)
constexpr int numImageFits = 3;

struct Widget
{
    int id = 0;                     // unique dans la Façade (sélection, annulation)
    WidgetType type = WidgetType::Knob;
    int x = 0, y = 0, w = 80, h = 80; // dans le canevas
    ParamId param = 0;              // 0 : pas de paramètre (étiquette seule, ou widget non lié)
    std::string text;               // étiquette
    std::vector<std::pair<ParamId, double>> snapshot; // Action : valeurs réelles (non normalisées) à restaurer
    int parent = 0;                 // identifiant de la zone qui contient ce widget, 0 : aucune
    int layout = 0;                 // Zone : ZoneLayout
    int align = 0, justify = 0;     // Zone : CrossAlign, Justify
    int gap = 8;                    // Zone : espace entre les widgets rangés
    int columns = 2;                // Zone en matrice : nombre de colonnes
    int channel = 1;                // Clavier : canal MIDI 1..16
    int firstNote = 36;             // Clavier : première touche affichée à gauche (do) ; le nombre de touches suit la largeur
    int hold = 0;                   // Clavier : 1, une touche cliquée reste enfoncée jusqu'au clic suivant
    int velocity = 100;             // Clavier : vélocité envoyée, 1..127
    int wheels = 0;                 // Clavier : 1, deux molettes (hauteur et modulation) à gauche des touches ; elles envoient des messages MIDI sur le canal du clavier
    // Potard et curseur : fonction, indépendante du style d'images.
    //  stepMode 0 : automatique (le curseur saute de cran en cran sur un paramètre à pas, le potard est continu) ;
    //  1 : toujours continu ; 2 : crans personnalisés, aux valeurs réelles de `notches` (une position par cran, à égale distance).
    int stepMode = 0;
    int invert = 0;                 // 1 : sens inversé
    // Échelle et courbe du lien (potard et curseur sur un paramètre continu) : comment la position du widget devient une valeur.
    //  scale 0 : l'échelle propre du paramètre ; 1 : linéaire en valeur ; 2 : logarithmique (rapport constant par distance) ; 3 : exponentielle.
    //  response : courbe libre (croissante) appliquée à la position avant l'échelle ; la droite d'origine ne change rien.
    int scale = 0;
    VelocityCurve response;
    std::vector<double> notches;    // valeurs réelles (celles du paramètre), triées, sans doublon
    midisend::Message midi;         // Midi : le message (canal 1..16 dans `midi.channel`)
    int midiTarget = 1;             // Midi : 1 Neptune (comme un clavier MIDI), 2 sortie MIDI de l'hôte, 3 les deux
    int midiFader = 0;              // Midi : 1, curseur (la valeur change avec le geste) au lieu d'un bouton
    int actionMode = 0;             // Action : 0 restaure l'instantané, 1 remet ses paramètres à leur valeur par défaut (annulation générale), 2.. une commande de combinaison (1 + commande, voir Combinations.h)
    int source = 0;                 // Voyant : 0 paramètre, 1 notes du canal `channel`, 2 notes de la piste
    int style = 0;                  // identifiant d'un ImageStyle de la Façade, 0 : aucun
    int vertical = 0;               // Curseur : 1, vertical
    std::vector<ParamId> mirror;    // Curseur, potard : autres paramètres qui reçoivent la même position (une boîte expressive qui commande plusieurs claviers)
    // Grille de scènes : `columns` (4..8) colonnes et `rows` (4..8) lignes de cases (une scène par case, de la première de la banque courante) ; `hue` : teinte 0..359 ;
    // `vivid` : de 0 (couleurs douces) à 100 (vives, pour la scène) ; `header` : 1, une barre avec la banque courante et ses flèches au-dessus des cases.
    int rows = 4;
    int hue = 30;
    int vivid = 55;
    int header = 1;
    int nameSize = 150, noteSize = 250;   // Grille de scènes : taille du nom (50..400) et des deux lignes de texte (50..500), en pour cent de la taille de base
    int setPill = 0;                       // Grille de scènes : 1, un bouton SET dans la barre de banque

    bool operator== (const Widget& o) const noexcept
    {
        return id == o.id && type == o.type && x == o.x && y == o.y && w == o.w && h == o.h && param == o.param && text == o.text && snapshot == o.snapshot
            && parent == o.parent && layout == o.layout && align == o.align && justify == o.justify && gap == o.gap && columns == o.columns
            && channel == o.channel && firstNote == o.firstNote && hold == o.hold && velocity == o.velocity && wheels == o.wheels
            && midi == o.midi && midiTarget == o.midiTarget && midiFader == o.midiFader && actionMode == o.actionMode && source == o.source && style == o.style && vertical == o.vertical && mirror == o.mirror && stepMode == o.stepMode && invert == o.invert && scale == o.scale && response == o.response && notches == o.notches
            && rows == o.rows && hue == o.hue && vivid == o.vivid && header == o.header && nameSize == o.nameSize && noteSize == o.noteSize && setPill == o.setPill;
    }
};

// Une Façade est un « livre » : la page décrite ici, plus les pages suivantes (`pages`, qui n'ont pas de pages à leur tour). Les pages ont chacune un nom
// (vide : « 1 », « 2 »...). Tout ce qui s'applique à une Façade (JSON, chemins d'images, rebranchement sur une autre piste) vaut pour toutes ses pages.
// La vue travaille sur une page à la fois (`pageOf` / `withPage`).
struct Facade
{
    std::string name;               // nom de cette page
    std::string image;              // chemin du fichier image (PNG / JPG) ; vide : fond uni
    ImageFit fit = ImageFit::Fit;
    std::vector<Widget> widgets;    // l'ordre est l'ordre d'empilement (le dernier est dessus)
    std::vector<ImageStyle> styles; // bibliothèque de styles d'images
    std::vector<Facade> pages;      // pages suivantes (la page 2 est pages[0]...)

    bool operator== (const Facade& o) const noexcept { return name == o.name && image == o.image && fit == o.fit && widgets == o.widgets && styles == o.styles && pages == o.pages; }
    bool operator!= (const Facade& o) const noexcept { return ! (*this == o); }

    // Identifiant libre pour un nouveau widget.
    int freeId() const noexcept;
    int freeStyleId() const noexcept;
    const ImageStyle* findStyle (int id) const noexcept;
    const Widget* find (int id) const noexcept;
    Widget* find (int id) noexcept;
};

// Crans personnalisés. Les positions sont celles des crans, à égale distance l'un de l'autre sur [0, 1] ; la valeur d'un cran est
// une valeur réelle du paramètre.
double notchPosition (const std::vector<double>& notches, double plain) noexcept;     // position du cran le plus proche de `plain`
double notchValueAt (const std::vector<double>& notches, double position) noexcept;   // valeur du cran le plus proche de `position`
// `n` crans (2..maxNotches) répartis régulièrement sur la course du paramètre (en tenant compte de sa courbe), sans doublon.
std::vector<double> evenNotches (const ParamSpec&, int n);

// Position du widget (0..1, sens de l'écran, hors inversion) -> position normalisée du paramètre, selon l'échelle et la courbe du widget ;
// et la réciproque. Sans effet sur un paramètre qui n'est pas continu.
double widgetNormalised (const Widget&, const ParamSpec&, double position) noexcept;
double widgetPosition (const Widget&, const ParamSpec&, double normalised) noexcept;
constexpr int numWidgetScales = 4;

// Dimensions par défaut d'un nouveau widget du type donné.
Widget defaultWidget (WidgetType type, int id, int x, int y);

// Outils de mise en page sur un ensemble de widgets (identifiants). Sans effet sous deux widgets (aligner, répartir) ou si la
// sélection est vide ; le résultat reste dans le canevas.
enum class Align { Left, Right, Top, Bottom, CentreH, CentreV };
void alignWidgets (Facade&, const std::set<int>& ids, Align);
void distributeWidgets (Facade&, const std::set<int>& ids, bool horizontal); // espaces égaux entre widgets (≥ 3)
void arrangeGrid (Facade&, const std::set<int>& ids, int columns);
// Répartit les widgets à angles égaux sur un cercle (sens horaire) dont le centre et le rayon viennent de l'espace qu'ils occupent.
// `refId` (un des widgets) va à la place `start` ; les autres suivent dans l'ordre où ils sont déjà autour du centre.
void arrangeCircle (Facade&, const std::set<int>& ids, int refId, CircleStart start);           // matrice : pas régulier, dans l'ordre d'empilement

// ---- Zones ----
// Tous les widgets contenus (directement ou non) dans la zone `id`.
std::set<int> descendants (const Facade&, int id);
// Rattache chaque widget de `touched` à la plus petite zone qui contient son centre (0 si aucune) ; si une zone a bougé ou changé de
// taille, les widgets libres ou déjà à elle sont réexaminés. Les autres rattachements ne changent pas.
void assignParents (Facade&, const std::set<int>& touched);
// Range le contenu de chaque zone ayant une disposition (ligne, colonne, matrice) ; déplace les zones imbriquées avec leur contenu.
void applyLayouts (Facade&);
// assignParents puis applyLayouts : à appeler après toute modification de géométrie.
void arrange (Facade&, const std::set<int>& touched);
// Déplace le widget (et le contenu d'une zone) de dx, dy.
void moveWidgets (Facade&, const std::set<int>& ids, int dx, int dy);
// Copie les widgets (avec le contenu des zones) décalés de `offset` ; rend les nouveaux identifiants.
std::set<int> duplicateWidgets (Facade&, const std::set<int>& ids, int offset);
// Donne à tous les widgets du même type que `id` la taille de ce widget.
void matchSize (Facade&, int id);

// Bornes appliquées : widgets dans le canevas et pas trop petits, identifiants uniques, textes courts, nombre limité.
// Source d'un vumètre, codée dans Widget::source : 0 la piste de la Façade, 1 le Master, 2 la réduction de gain du compresseur de la piste de
// la Façade ; 100 + i piste i, 200 + i bus i, 300 + i piste parallèle i (niveaux) ; 400 + i, 500 + i, 600 + i les réductions de gain
// des compresseurs de la piste, du bus ou de la piste parallèle i ; 700 la réduction de gain du compresseur du Master.
struct MeterSource
{
    enum class Kind { Track, Bus, Parallel, Master };
    Kind kind = Kind::Track;
    int index = -1;               // -1 : la piste de la Façade
    bool reduction = false;       // réduction de gain du compresseur au lieu du niveau
    bool operator== (const MeterSource& o) const noexcept { return kind == o.kind && index == o.index && reduction == o.reduction; }
};
MeterSource decodeMeterSource (int source) noexcept;
int encodeMeterSource (const MeterSource&) noexcept;
int sanitisedMeterSource (int source) noexcept;   // une source inconnue ou hors bornes devient 0

constexpr int maxPages = 16;
Facade sanitised (Facade f);
// ---- Pages d'une Façade ----
int pageCount (const Facade& book) noexcept;                       // au moins 1
std::vector<Facade> splitPages (const Facade& book);                // chaque page seule (sans `pages`)
Facade joinPages (std::vector<Facade> pages);
Facade pageOf (const Facade& book, int index);                      // la page `index` (bornée), sans les autres
Facade withPage (const Facade& book, int index, Facade page);       // remplace le contenu et le nom de la page
Facade addPage (const Facade& book, std::string name);               // une page vide à la fin (rien si 16 pages)
Facade removePage (const Facade& book, int index);                   // jamais la dernière
Facade renamePage (const Facade& book, int index, std::string name);
Facade movePage (const Facade& book, int index, int target);

std::string toJson (const Facade&);
Facade fromJson (std::string_view text); // lecture tolérante : champ invalide ou absent -> défaut ; bornes appliquées
} // namespace neptune::core::facade
