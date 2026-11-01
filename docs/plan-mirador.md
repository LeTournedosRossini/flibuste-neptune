# Plan : Mirador (nom provisoire), plugin de mesure et de visualisation

Écrit le 2026-10-30. Décision de Jean : un seul plugin, sonie de mastering et outil de mixage, libebur128, réutiliser les visualisations existantes. Aucun code écrit à ce stade.

## Constat : une bonne partie existe déjà

- **libebur128 v1.2.6 (MIT)** est déjà intégré (`cmake/Dependencies.cmake`, option `NEPTUNE_WITH_EBUR128`). Pas de nouvelle dépendance.
- **`LoudnessMeter`** (`core/.../Loudness.h`, `core/src/Loudness.cpp`, `tests/test_loudness.cpp`) mesure déjà pour le Master de Neptune : momentanée, à court terme, intégrée, crête vraie (dBTP). Thread d'analyse séparé, tampon circulaire sans verrou côté audio. Il faut l'étendre, pas le refaire.
- **`AnalyzerRing`** (anneau audio → interface) et **`NovaHistoryView`** (historique défilant) viennent du compresseur.
- **`SpectrumAnalyzer`** + **`NovaSpectrum`** viennent de l'égaliseur.
- **`VMeter`** (NovaKit) est le mètre vertical commun.
- **`Oversampler`** et **`Biquad`** existent.

Ce qui manque : LRA, historique de sonie sur plusieurs minutes, mesures de mixage (RMS, crête, facteur de crête, corrélation, vectorscope), et un plugin qui les montre.

## Principe

Effet **transparent** : l'audio passe sans changement, aucune latence, pas de traitement. Le plugin mesure et affiche. Entrée stéréo, sortie stéréo. Entrée latérale facultative (non retenue pour la première version).

## Mesures

**Sonie (page SONIE)**
- Momentanée (400 ms), à court terme (3 s), intégrée (porte absolue −70 LUFS, porte relative −10 LU), gérées par libebur128.
- Plage de sonie (LRA), via `ebur128_loudness_range`. À ajouter à `LoudnessMeter`.
- Crête vraie en dBTP (suréchantillonnage par libebur128). Maximum tenu.
- PLR (crête vraie moins sonie intégrée) et PSR (crête vraie moins sonie à court terme), calculés par nous à partir des valeurs précédentes.
- Cible choisie (−23 LUFS EBU R128, −14 streaming, −16 podcast, −24 ATSC A/85, libre) : écart affiché en LU, couleur selon la tolérance (±1 LU par défaut).
- Remise à zéro de l'intégrée à la main ; option de remise à zéro au départ du transport de l'hôte.
- Historique de sonie en courbe défilante (réutilise `NovaHistoryView`), durée réglable (1 à 60 minutes).

**Niveau (page MIXAGE)**
- Crête et RMS par voie, rétention des crêtes, échelle en dBFS (VMeter existants), voyant d'écrêtage.
- Facteur de crête (crête − RMS).
- Historique d'amplitude défilant, copié sur le compresseur.

**Stéréo (page STÉRÉO)**
- Corrélation de phase (−1 à +1) en barre et en historique.
- Vectorscope (Lissajous, rotation M/S, intensité qui s'estompe).
- Équilibre gauche/droite, largeur (rapport M/S).
- C'est le bon endroit pour vérifier à l'œil les polarités inversées de Diapason.

**Spectre (page SPECTRE)**
- Réutilise `SpectrumAnalyzer` et `NovaSpectrum`. Courbe moyennée, crête tenue, lissage en fractions d'octave.
- Tiers d'octave (RTA) en option, après la première version.

## Interface

Même style NOVA, pages : SONIE, MIXAGE, STÉRÉO, SPECTRE, SETTINGS. Mètres à gauche et à droite comme dans les autres plugins. Réglages : cible de sonie, tolérance, durée de l'historique, temps de retombée des crêtes, mise à zéro automatique. Tous les réglages sont des paramètres d'hôte, mais les mesures ne sont pas des paramètres. Pas de presets ni de comparaison A/B (comme Diapason).

## Cœur et adaptateur

- Cœur (`core/.../LevelMeter*.h/.cpp`, sans JUCE) : `LoudnessMeter` étendu (LRA, maximum de crête, PLR/PSR, historique par seconde), nouveau moteur de mesures de niveau et de stéréo (crête, RMS, corrélation, nuage de points pour le vectorscope), paramètres.
- Adaptateur : `MiradorPlugin.h/.cpp` (copie du modèle d'un effet transparent), éditeur NOVA, CMake et workflow (sans lancer la CI).
- Le thread audio ne fait que copier et calculer des accumulateurs simples. Les calculs coûteux (libebur128, FFT) restent sur le thread d'analyse ou d'interface.

## Étapes

1. Cœur : extension de `LoudnessMeter` (LRA, maximum de crête, historique), mesures de niveau et de stéréo, paramètres. Tests.
2. Adaptateur : plugin transparent, état, CLAP.
3. Interface : pages SONIE, MIXAGE, STÉRÉO, SPECTRE, SETTINGS. Rendu hors ligne (NovaGallery) pour la mise en page.
4. Compilation Linux et Windows, installation, état des versions, message au banc (quand Jean le demandera).

## Vérifications prévues (ce que je peux vraiment contrôler)

- Signaux de référence de la norme (EBU Tech 3341) : un sinus de 1 kHz à −23 dBFS stéréo donne −23,0 LUFS ; sinus à −33 dBFS ⇒ −33,0 ; tests de la porte relative ; crête vraie sur signal à 4× suréchantillonné. Valeurs attendues calculées indépendamment, tolérance ±0,1 LU.
- LRA sur un signal par paliers de niveau connus.
- Corrélation : +1 pour deux voies identiques, −1 pour une voie inversée, ≈0 pour deux bruits indépendants.
- Transparence : la sortie est bit à bit égale à l'entrée.

## Limites et risques

- Rien n'est écouté ni regardé dans Reaper de mon côté. La précision des mesures se vérifie par calcul. La lisibilité des affichages (échelles, couleurs, vitesse) se juge à l'écran.
- Nous ne prétendons pas à une certification de conformité : la bibliothèque est conforme à BS.1770-4, mais le plugin n'a pas passé un banc d'essai officiel.
- La sonie intégrée dépend de la fréquence d'échantillonnage : le compteur est redémarré quand elle change.
- Le LRA et l'intégrée demandent de la mémoire qui grandit avec la durée : libebur128 conserve des blocs de 100 ms. Une longue session (plusieurs heures) reste raisonnable, mais je mettrai une limite et je la testerai.
- Le vectorscope sur un grand nombre de points peut coûter en dessin : on limite le nombre de points par image.
- Pas de mesure multicanal (5.1) ni de sonie par entrée latérale dans la première version.

## Questions ouvertes pour Jean

1. Nom : Mirador : nom retenu par Jean le 2026-10-30.
2. Une entrée latérale (mesurer une autre piste) est-elle utile ?
3. (Hors cadre.) Faut-il une courbe de sonie qui se superpose à la forme d'onde du projet ? Cela exige une analyse hors ligne et sort du cadre de cette première version.
