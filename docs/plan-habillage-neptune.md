# Plan : habillage de Neptune au style NOVA (2026-11-01)

Décision de l'utilisateur : **habillage seulement** (pas de refonte de la mise en page). Inventaire fait par lecture du code ; aucune capture d'écran vue pour l'instant.

## État actuel
- La fenêtre `NeptuneEditor` (`PluginEditor.cpp/.h`, 1160 x 960 logique, racine mise à l'échelle par `ScaledRoot`) utilise déjà `nova::Skin` (palette, jetons) et `nova::LookAndFeel` (menus, info-bulles, défilement, boutons JUCE standards) et `NeptuneKnob` (style Led).
- Elle n'utilise **aucune** brique de mise en page du kit : ni `paintHeaderBar` / `paintBottomBar`, ni `Panel`, `Pill`, `Switch`, `NavItem`, `KnobCard`, `VMeter`, `InfoButton`.
- En-tête actuel (`paintContent`) : titre « Neptune » en texte 28 pt, version en 10 pt dessous, puis une rangée de `juce::TextButton` (vues, MIDI, OSC, presets globaux, console, split, annuler/refaire, panique, nombre de voix, profil, latence). Pas de logo, pas de bandeau, pas de ligne de séparation.
- Les vues sont dessinées à la main avec les jetons de couleur (`Token::BgCard`, `Border`...) : `MixerView` (1487 lignes, bandes de piste avec cartes à 8 px de rayon), `ModView`, `RouterView`, `FacadeView` (3908 lignes), `ControlPanel`, `ChokePanel`, `AudioInView`, `MidiFilterEditor`, `MidiMessagePanel`, `CombinationsPanel`, `ImportWizard`, `InsertOverlay`, `KnobModal`. Rayons, épaisseurs de trait, tailles de police et marges propres à chacune.
- Déjà au style NOVA : l'éditeur Synthé / FM intégré (`InstrumentPanel`), les inserts (`InsertOverlay` ouvre les éditeurs NOVA).

## Ordre proposé
1. **En-tête et pied** : `paintHeaderBar` (logo, nom, sous-titre, version) et `paintBottomBar` autour de la fenêtre ; les boutons de la rangée du haut deviennent des `nova::Pill`, le choix de piste reste un menu habillé ; la bascule Mixeur / Mapping / Façade / Modulation / Routeur devient des `NavItem` (comme les pages des autres plugins). Annuler, refaire, panique en `Pill`.
2. **Mixeur** : cartes de bande sur le modèle `Panel` ; `VMeter` pour les niveaux ; `Pill` pour mute / solo / MIDI ; mêmes rayons et épaisseurs.
3. **Modulation, Routeur** : cartes et pastilles du kit.
4. **Façade** : seulement les éléments communs (barre d'outils, panneaux) ; le dessin de l'instrument lui-même reste le sien.
5. **Calques** (filtre MIDI, envoi MIDI, combinaisons, assistant d'import) : en-têtes et boutons du kit.

Compiler et installer après chaque étape ; l'utilisateur regarde dans Reaper entre deux.

## Points à demander
- Captures d'écran de Neptune (Mixeur, Mapping, Façade, Modulation, Routeur, un calque) et d'un plugin NOVA de référence (Fournaise ou Délai), pour fixer ce qui compte le plus.
- Le logo : le dégradé accent / rose du kit convient-il à Neptune, ou Neptune a-t-il son identité ?
- La hauteur de la fenêtre : l'en-tête NOVA prend de la place ; la fenêtre de 960 de haut la supporte-t-elle ?

## État au 2026-11-01 (soir)
Fait, installé sous Linux et Windows, regardé dans Reaper par Jean (« très bien ») :
1. **En-tête et pied NOVA** : logo, NEPTUNE, version en sous-titre (`nova::HeaderStyle::Window`) ; pied NOVA derrière la rangée du bas. Les boutons de la barre du haut gardent le dessin des `TextButton` du LookAndFeel NOVA, déjà identique à celui des pastilles ; ils n'ont pas été remplacés par des `Pill`.
2. **Éditeurs intégrés** (Synthé, FM, Orgue, Sampler) : option `embedded` : en-tête « Compact » (nom de l'instrument seulement, sans logo, sous-titre ni version). Leur bande d'en-tête et leur pied sont gardés : ils portent le menu de presets, le A/B, REC et PLAY.
3. **Mixeur** : conteneur « pistes » (défilement, zoom) et conteneur « bus » avec le master à leur droite, de la hauteur d'une bande, sur deux colonnes (140 de large : plus large qu'avant, pour tenir sonie, Trim, Pan, fader et mètres). Largeur de conception 900 -> 1110.
4. **MOD** : trois conteneurs (LFO, enveloppes, routes), une carte par route. **Routeur** : un conteneur, une carte par règle.
5. **Façade** : barre d'outils dans un conteneur arrondi, cadre du canevas à coins arrondis ; le dessin des widgets de l'utilisateur n'est pas touché.
6. **Calques** : fenêtres des combinaisons, de l'envoi MIDI et de la recherche de paramètres rendues opaques (le fond PanelBg du thème est translucide : on voyait le Mixeur à travers) ; boutons du filtre MIDI élargis (« + Condition » était coupé) ; page Probe dans un conteneur.

Reste : assistant d'import (non regardé), éventuellement l'en-tête compact plus bas, une barre de navigation en `NavItem` à la place des onglets de texte (non demandé).

Contrôle visuel sans captures de l'utilisateur : `Neptune_Standalone` (cible de `build-local`) lancé sous Xvfb, capture par `xwd -root` (24 bits par pixel), clics et redimensionnement par python-xlib (XTEST), le tout dans un script lancé par `claude-build linux` qui tue Xvfb à la sortie. Les mètres restent vides (pas d'audio).

## Sélecteur de thème, de contraste et de langue (2026-11-02)
Un bouton « Theme » dans la barre du haut (après « MIDI send ») ouvre un menu : thème (Dark, Light, High contrast, Wood), contraste (0 à 4, grisé pour High contrast), langue (English, Français). Le choix est écrit dans `settings.json` (`NeptuneProcessor::uiSetLook` ; la bascule du profil Live / Studio, elle, n'y est jamais écrite) et **l'éditeur est reconstruit** avec la nouvelle palette, en restant sur la même page. Pour cela l'éditeur que reçoit l'hôte est maintenant un cadre (`NeptunePluginEditor`) qui contient `NeptuneEditor` (devenu un composant simple) et le recrée : toutes les vues gardent leur propre copie de la palette, reconstruire est plus sûr que de les mettre à jour une à une. Largeur logique minimale de la fenêtre passée de 1100 à 1180 (la barre du haut a un bouton de plus). Un seul réglage par machine : les autres instances de Neptune ouvertes ne changent qu'à la réouverture de leur fenêtre. Vérifié sur le Standalone (thème Clair, retour au Bois, `settings.json` identique), pas encore dans Reaper.

## Retouches du Mixeur (2026-11-02)
- **Thème par défaut : Dark** (le défaut du code l'était déjà ; `settings.json` de Jean, Linux et Windows, remis sur « dark »). Thème, contraste et langue restent un choix par machine, par le bouton « Theme ».
- **Faders du mixeur** : la poignée n'est plus le grand rectangle blanc mais un gris moyen (le texte tiré vers le fond) avec un liseré clair et un trait de prise ; les points des curseurs de pan et de trim sont grisés de la même façon (propriété `soft`, seulement dans le mixeur).
- **Piste sélectionnée** : plus de voile orange (`Nav1`), le fond reste celui des autres bandes ; seul le filet orange la marque, comme le Master. Le même principe vaudrait ailleurs (lignes sélectionnées de listes : fond d'accent à 35 %), non touché pour l'instant.
- **Master** : une marge de deux pixels entre le mètre de droite et le filet orange (le même ordre que l'écart entre les deux mètres).

## Boîtes et menus hors de la fenêtre (2026-11-02)
`nova::DialogLook` (NovaKit) : tant qu'un éditeur Neptune est ouvert, le style NOVA est le style par défaut de JUCE ; les boîtes de renommage et de message (`AlertWindow`) et les menus sans habillage propre l'ont donc aussi (fond de carte, boutons NOVA). Le dernier éditeur fermé rend le style d'origine. Regardé sur le Standalone (renommage d'une scène) ; les autres boîtes passent par le même style. L'assistant d'import (`ImportWizard`) garde ses jetons de couleur, non revu à l'écran.

## Diapason : page MIDI (2026-11-02)
Troisième page avant RÉGLAGES : type de message (note on / off, CC, programme, pitch bend, pressions, SysEx), canal et valeurs en pastilles à glisser avec « - / + » et saisie au double-clic, « Envoyer », « Envoi à chaque changement » (pas pour les notes ni le SysEx), « Toutes notes off » (CC 123), historique. Les messages sortent par la sortie MIDI CLAP du plugin (comme l'envoi MIDI de Neptune : `postMidi`, `addOutboundEventsToQueue`) ; le tampon MIDI reste vidé après usage, aucun écho. Regardé sur le Standalone (le Standalone n'a pas de sortie MIDI : le journal montre l'envoi) ; la sortie vers un appareil ou une piste reste à essayer dans Reaper.
