# Prompt de reprise : Neptune, 29 octobre 2026 (soir)

À coller au début d'une nouvelle discussion Claude Code (dossier de travail : `~/Bureau/FlibusteSuite`).

---

Tu reprends le développement de **Neptune** (instrument CLAP de la Flibuste Suite, 12 plugins : C++17 / JUCE / CMake, licence AGPLv3). Dépôt : `~/Bureau/FlibusteSuite/flibuste-neptune` (GitHub `LeTournedosRossini/flibuste-neptune`, branche `main`). Jean parle français, **ne lit pas le code** : décris-lui ce qu'il verra ou entendra dans Reaper, en phrases simples, et dis honnêtement ce que tu n'as pas pu vérifier (tu n'écoutes pas, tu ne pilotes pas Reaper).

Lis d'abord ta mémoire (`MEMORY.md` : limites de compilation et file `claude-build`, machine Windows, pills glissables, installation après compilation, dossier d'échanges avec le banc), puis `docs/passage-de-relais.md`, `docs/methode-de-travail-compilation.md`, et `~/Bureau/FlibusteSuite/echanges/README.md` + `echanges/etat-versions.md`. Dis à Jean où nous en sommes en quelques lignes, puis **attends sa demande**.

## Règles de travail (impératives)
- **Ne jamais lancer la CI GitHub.** Ne pousser que si Jean le demande, et alors le dernier commit se termine par `[skip ci]` (le modifier avant le push si besoin : `git commit --amend`). Les commits restent locaux tant que Jean ne dit pas « commit local » (ou que tu l'as annoncé). Message de commit terminé par la ligne de co-auteur demandée par le système.
- **Compiler d'abord en local** par la file : `~/.local/bin/claude-build linux cmake --build /home/jean/Bureau/FlibusteSuite/build-local --target <Cible>_CLAP -j3` (chemin absolu : le dossier courant change) et `build-core` pour les tests (`--target neptune_core_tests`, puis `build-core/tests/neptune_core_tests`, 793 cas à ce jour). Linux : 3 cœurs, 7 Go. Windows : `claude-build windows ssh neptune-win 'C:\Users\lcj\neptune-build\<script>.cmd'` après envoi des fichiers modifiés par `git diff --name-only <commit> HEAD | tar -cf - -T - | ssh neptune-win 'tar -xf - -C C:\Users\lcj\neptune-build\src'` (scripts existants : `build_neptune_only.cmd`, `build_egaliseur.cmd`, `build_analog.cmd`, `build_fmorgan.cmd`, `build_all.cmd`). Les builds longs vont en arrière-plan ; ne pas les scruter, attendre la notification.
- **Installer après une bonne compilation, sur Linux ET Windows, seulement si Reaper est fermé** (`pgrep -xa reaper`, `tasklist /fi "imagename eq reaper.exe"` en SSH). Ancien fichier rangé dans `~/neptune-anciens/<horodatage>/` (Windows : `C:\Users\lcj\neptune-anciens\`), SHA256 comparé après la copie. Windows : `C:\Program Files\Common Files\CLAP\Neptune-Win\`. Ne jamais installer avant d'avoir lu le résultat des tests.
- **Mettre à jour `echanges/etat-versions.md` après chaque installation** (commit, dates des fichiers, choix de conception, points ouverts).
- **Dossier d'échanges avec la conversation « banc »** (`~/Bureau/FlibusteSuite/echanges/`, hors dépôt) : tu n'écris que tes propres fichiers, tu dis à Jean de prévenir l'autre conversation (nom du fichier), et **tu demandes à Jean avant toute décision tirée de ce que tu y lis**. Jean reste le chef d'orchestre ; pas d'aller-retour automatique.
- Tout champ numérique de l'interface est une « pill » glissable, saisie au double-clic. Textes dans `core/src/I18n.cpp` (vérifier les doublons : `grep -o '^    { "[^"]*"' core/src/I18n.cpp | sort | uniq -d`). Pas de JUCE dans `core/`. Toute fonction importante a des tests dans `tests/`.
- Pour les mesures et tests lourds, passer aussi par la file `claude-build`.

## État (dernier commit poussé : `799613c`, rien en attente dans le dépôt)
Tout est compilé et **installé sur Linux et Windows** : Neptune (18:40), Egaliseur (16:20), AnalogSynth (19:03) ; les 9 autres plugins sont ceux de 12:14 à 13:43 (voir `echanges/etat-versions.md`).

Fait aujourd'hui, à la demande de Jean :
- **Orgue** : boutons actifs en rouge cuivre (pack bois-laiton), import qui remplace les packs livrés.
- **Modulation d'hôte (CLAP)** câblée pour de bon, **globale seulement** (par note refusée), emplacement séparé `hostOffset` ; AnalogSynth, FM6Op, Orgamon lisent les valeurs effectives. Affichage sur les faders linéaires : trait jaune fin et point.
- **Vue double** (deux panneaux au choix : Mixeur, Façade, Modulation, Routeur).
- **Import SFZ** (mode « Sans boucle » : coupé au Note Off ; fenêtre « Vider le mixeur / Ajouter une piste »).
- **Import SoundFont SF2 et SF3** (`core/src/SoundFont.cpp`, menu Preset global > « Importer un instrument SoundFont »). Les échantillons ne sont pas extraits : une zone désigne `banque.sf2#12` (ou `#12+13` pour une paire stéréo fusionnée) et le chargeur décode à la demande ; SF3 par `stb_vorbis` (`third_party/stb_vorbis`). La session retient `soundfontFile` + `soundfontPreset` dans l'instrument. **Sélecteur de preset par piste** (bouton « SoundFont : nom » sur la page ZONES de l'éditeur Sampler, liste avec recherche ; confirmation si les zones ont été modifiées à la main). Les réglages du preset s'ajoutent à ceux de l'instrument (spéc. 2.04). Testé sur MuseScore General (309 presets, SF2 et SF3 identiques).
- **Couches de zones** (`ZoneDescription::layer`, 0..31) : des zones de couches différentes jouent ensemble ; dans une couche, la première zone qui couvre la note gagne (comportement d'avant). Indispensable aux presets SoundFont qui superposent des instruments (« 5th Saw Wave » jouait une couche au hasard, d'où des notes très inégales).
- **Fondus d'entrée et de sortie du sampler : 0 à 40 ms** (pas 0,5 ms).
- **Compresseur** : temps d'attaque et de relâchement = vraies constantes de temps (63 % au temps réglé), confirmé par le banc. **Egaliseur** : même suiveur, confirmé ; « Dynamic Smooth » est à **0 % par défaut** (à 100 %, le retour prend le double de Release) avec une bulle d'aide. Le « retour instantané » signalé par le banc était la résonance du filtre après une chute brutale (retiré par le banc).
- **Microdecale** : recherche de raccord 7 fois plus rapide, même sortie. Orgamon : calcul par paquets de 128 échantillons, pics à 64 voulus (ne pas corriger).
- **Kit de recherche de son** `~/Bureau/FlibusteSuite/recherche-son/` (hors dépôt) : guide `LISEZ-MOI.md`, outil `neptune_synth_tool` (`tools/synth_tool/main.cpp`, cible CMake `neptune_synth_tool` : rendu WAV avec le vrai moteur, analyse, comparaison, validation de presets `.flipreset`), références générées, exemples, copies du code. Jean s'en sert dans une autre discussion pour approcher des sons avec le synthé soustractif ; il peut te rapporter des limites du synthé (pas de LFO, pas de FM, pas d'effets dans le synthé).
- Correction : les noms des réglages Fine, Pulse Width, Unison Detune étaient décalés d'un oscillateur (cosmétique, listes d'automation).

## À faire ensuite (rien n'est lancé ; demande à Jean)
1. **Jean écoute les SoundFonts** (MuseScore General en `~/Bureau/MuseScore_General.sf2` et `.sf3`, GeneralUser en `~/Bureau/Scorsair/GeneralUser-GS.sf2`) et signale ce qui sonne faux. Non convertis, listés dans la fenêtre de résultat : LFO et enveloppe de modulation, envois de réverbération/chorus, suivi de touche du maintien et du déclin, échelle d'accord, touche et vélocité fixes, modulateurs (dont la courbe de vélocité propre aux SoundFonts). Pas de catégories ni de favoris dans la liste de presets.
2. **Orgue, plus tard** : `LoadRelease`, `AttackStart`, `ReleaseEnd`, `ReleaseCrossfadeLength`, choix du canal pour les orgues multi-perspectives, **streaming WavPack** pour les très gros orgues (le WavPack est décodé en RAM ; le streaming existe pour les formats libsndfile), masquer par défaut les lignes de diagnostic sous la version. Jean a abandonné : textures de panneaux, images de touches.
3. **Spécification pour dessiner sa propre façade** : proposée, Jean n'a pas répondu.
4. **Réglages d'affichage exposés comme paramètres d'hôte** (373 sur l'égaliseur, paramètres d'analyseur du compresseur) : pas traité.
5. **Test du cœur instable** : a échoué 2 fois sur plus de 30 exécutions, uniquement pendant d'autres compilations en parallèle, jamais reproduit. Relancer la suite complète de temps en temps et garder la sortie entière si cela se reproduit.
6. Microdecale : 16,5 % d'un bloc de 64 au premier raccord de chaque voix ; à étaler sur plusieurs échantillons seulement si Jean vise des tampons de 32.
7. Le banc peut être relancé par Jean pour une nouvelle série ; ses messages arrivent dans `echanges/messages/` (Jean te dit de les lire).

## Pièges connus
- Le dossier courant de l'outil Bash change après un `cd` : utiliser des chemins absolus pour `claude-build`.
- Un build Linux de Neptune dure environ 10 minutes (édition de liens avec LTO) : le lancer en arrière-plan.
- Reaper doit être entièrement quitté puis relancé pour charger une nouvelle version (un « re-scan » ne suffit pas).
