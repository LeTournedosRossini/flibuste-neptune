# Essai d'Arpège avec Carte (5 novembre 2026)

Tout est déjà dans les plugins : un Arpège neuf contient les motifs d'essai dans ses emplacements 12 à 15, et Carte a un bouton qui charge un morceau d'essai.

## En trois gestes

1. Sur une piste, mettez **Carte**. Cliquez **« Charger le morceau d'essai »** (barre du haut). Huit mesures apparaissent : accords (C, Am7, F, G7, Dm7, Am7/D, C, Cmaj7), gammes, tonalité, et la voie « Motif » qui appelle les motifs 12 à 15. La dernière mesure est en 3/4. Il commence à la tête de lecture (placez-la au début du projet avant de cliquer).
2. Sur la piste suivante (ou la même piste, après Carte), mettez **Arpège** : réglez **Source : Carte** et mettez un instrument après lui.
3. Lancez la lecture. Arpège joue seul, sans touche tenue. Le bas de sa fenêtre dit « Joue : les motifs placés dans Carte » et donne l'accord en cours.

## Ce que vous devez entendre, mesure par mesure

| Mesures | Motif appelé par Carte | Ce qu'il montre |
|---|---|---|
| 1-2 | 12 « Essai 1 » (mode notes) | huit notes montantes puis redescendantes, **vélocité** croissante (50 à 120), **durées** alternées (courte, longue), **accents** sur les 3ᵉ et 7ᵉ notes ; les rangs viennent de l'accord de Carte (do majeur : do mi sol, la 4ᵉ remonte d'une octave) |
| 3-4 | 13 « Essai 2 » (mode accord, degrés) | arpège sur les degrés 1, 3, 5, 7, 9 ; **la 2ᵉ moitié de chaque mesure est parfois remplacée** par une variante (une fois sur deux, toujours la même à chaque lecture) ; sur F (mesure 3) le degré 7 et le 9 n'existent pas : voir « Degré absent » ci-dessous |
| 5-6 | 14 « Essai 3 » (ballade) | basse + accord entier, accents, vélocités différentes, durées courtes aux temps faibles ; mesure 6 : Am7/D, la **basse est ré**, l'accord reste la mineur 7 |
| 7-8 | 15 « Essai 4 » (deux mesures) | motif de **deux mesures** (1, 5, 3, 5 puis des triolets puis un accord) : la mesure 7 joue sa 1ʳᵉ moitié, la mesure 8 (3/4, plus courte) joue la 2ᵉ, **coupée à la barre** |

## À essayer

- **Degré absent** (rangée du bas d'Arpège) : « Sauter » (le degré manquant ne joue pas), « Jouer » (la note par défaut du degré), « Gamme » (la note de la gamme de Carte : sur Dm7 en ré dorien ou sur F en do majeur). Écoutez la mesure 3 et la mesure 5 avec les trois choix.
- **Matrice** : choisissez « Motif dessiné » 12, 13, 14 ou 15 : les bandes sous la matrice montrent la **vélocité** et la **durée** de chaque coup (glisser pour régler, clic droit pour revenir au réglage du plugin). **Annuler** / **Rétablir** (ou Ctrl+Z / Ctrl+Y) défont tout.
- **Octave de base** : où Arpège joue quand rien n'est tenu.
- **Notes tenues** : tenez une note pendant la lecture : elle l'emporte sur l'accord de Carte (elle sert de repère d'octave).
- **Carte** : déplacez les cartes à la souris ou aux flèches ; étirez par les deux bords ; double-clic pour changer un accord (constructeur) ; sélection au rectangle, Ctrl+C / Ctrl+V / Ctrl+D.
- Source « Notes tenues » : Arpège ne lit plus Carte ; il joue ce que vous tenez, avec le motif choisi.

## Les fichiers

- `accords-essai.json` : les mêmes accords et mesures que le morceau d'essai, au format d'échange de Scorsaire (`flibuste-accords`). Carte : **Importer des accords...** (remplace les accords et les barres, garde gammes, tonalités et motifs). Utile pour vérifier l'import et le 3/4.
- `motifs-essai.json` : les quatre motifs d'essai (`flibuste-motifs`). Arpège : choisissez un « Motif dessiné » puis **Importer** (le fichier remplit l'emplacement choisi et les suivants). Utile pour vérifier l'import et l'export de motifs, et la conservation de `vel`, `gate` et des variantes.

## Orgamon suit la carte

Avec la même Carte (le morceau d'essai), mettez **Orgamon** sur une autre piste. Dans sa page **ORGUE**, le bouton « Carte : ... » (sous le bouton des contrôleurs) ouvre un menu : cochez **Suivre les accords de Carte**. Lancez la lecture : l'orgue tient l'accord en cours, sans touche, et change d'accord à l'instant exact. Le bouton affiche « Carte : ON · canal 1 · Dm7 ». Réglages du menu : canal de Carte, canal MIDI des accords (celui d'un clavier de l'orgue : 1, 2 ou 3 selon votre routage), canal MIDI de la basse (pédalier ; « aucune basse » par défaut), octave de base. À l'arrêt de la lecture, tout est relâché.

## Mise à jour du 6 novembre : voix conduites, grattage, approche, coups étouffés, bibliothèque

- **Voix conduites.** Orgamon, AnalogSynth et FM6Op (menu « Carte : ... ») posent chaque accord au plus près du précédent (notes communes gardées, voix du haut calme, pas de seconde mineure). Arpège fait de même pour les coups dont `voices.rh` vaut `led` dans le fichier de motif. « Position fixe » redonne l'ancien comportement.
- **Grattage** (Arpège et menu Carte des instruments, sauf Orgamon) : délai entre les notes, coup bas / haut en alternance, vélocité décroissante.
- **Approche** : accord de dominante, de triton, chromatique ou parallèle placé avant un changement d'accord.
- **Coups étouffés** : un coup peut être marqué étouffé (`x.arpege.muted` dans le fichier) ; il sonne court et faible. Dans la matrice d'Arpège, bande « Muted ».
- **Motifs appelés par Carte dans les instruments** : le menu Carte choisit le motif pour les claviers (haut, bas ou les deux) et pour la pédale.
- **Bibliothèque de motifs** : dossier `~/.config/Neptune/motifs/` (Windows : `%APPDATA%\Neptune\motifs`), un motif par fichier. Arpège y exporte par défaut ; les instruments y choisissent, avec Importer et Exporter dans leur menu. Neuf exemples dans `docs/exemples-motifs/` (« Essai orgue - 1 » à « 9 »), à copier dans ce dossier.
- **Canaux MIDI par défaut** : clavier haut 1, bas 2, pédale 3.
