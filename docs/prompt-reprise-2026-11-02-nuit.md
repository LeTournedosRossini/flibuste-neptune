# Prompt de reprise — Flibuste Suite / Neptune (2026-11-02, nuit)

Tu reprends le travail sur la Flibuste Suite (Neptune, sampler / orgue / synthé / console de scènes en CLAP, plus les plugins NOVA : Digue, Diapason, etc.). Jean (l'utilisateur) est musicien et enseignant ; il parle français, dicte souvent (le texte contient des erreurs de reconnaissance vocale : « les boutons 7 » = « les boutons Set »). Réponds en français, sans jargon, en disant d'abord ce qui est fait et ce qui est vérifié ou non.

## Règles qui ne bougent pas
- **Ne jamais pousser sur GitHub** sans demande explicite de Jean (quota). Commits locaux seulement, avec `[skip ci]` et la ligne `Co-Authored-By` demandée par le système.
- **Reaper doit être fermé** avant toute installation d'un plugin, sur la machine concernée (`pgrep -xa reaper` sous Linux ; `tasklist /fi "imagename eq reaper.exe"` par SSH sous Windows). Une fois, un plugin a été copié Linux alors que Reaper tournait : ne pas recommencer. Ancien fichier sauvegardé dans `~/neptune-anciens/<horodatage>/` (Windows : `C:\Users\lcj\neptune-anciens\`), SHA256 comparé après copie.
- **Après une compilation réussie, installer** (Linux et Windows) sans redemander, si Reaper est fermé. Pour le Standalone Windows : copier `Neptune.exe` seulement si `Neptune.exe` n'est pas lancé.
- **Les plugins qui marchent ne doivent pas changer de comportement** : le code propre au Standalone est derrière `#ifdef NEPTUNE_STANDALONE_APP`.
- Compilations par la file `~/.local/bin/claude-build linux|windows <commande>` (Linux : 3 cœurs / 7 Go ; Windows : tous les cœurs). `build-core` = noyau + tests (plugins OFF) ; `build-local` = plugins ON (avec `-DNEPTUNE_BUILD_STANDALONE=ON` dans son cache).
- Jean fait le lien avec la conversation « banc » (mesures des plugins) ; tu tiens `echanges/etat-versions.md` (une ligne par plugin, avec heures d'installation Linux / Windows). Demander à Jean avant toute décision venant du banc.
- Ne pas modifier `~/.config/Neptune/settings.json` (thème, contraste, langue de Jean) ; mes essais d'écran le restaurent à l'identique.

## Où est tout
- Dépôt : `/home/jean/Bureau/FlibusteSuite/flibuste-neptune` (branche `main`, commits locaux). Cœur C++17 sans JUCE : `core/` ; adaptateurs JUCE / CLAP : `adapter-juce/src/` ; tests Catch2 : `tests/` (930 cas, tous verts à la dernière exécution) ; docs : `docs/` (`plan-scenes.md`, `plan-standalone.md`, `plan-habillage-neptune.md`, `plan-limiteur.md`, `passage-de-relais.md`...).
- Builds : `/home/jean/Bureau/FlibusteSuite/build-core`, `/home/jean/Bureau/FlibusteSuite/build-local`.
- Windows (SSH `neptune-win`, fonctionne seul) : sources reçues par `tar --exclude=.git -cf - . | ssh neptune-win 'tar -xf - -C C:\Users\lcj\neptune-build\src'` ; scripts dans `C:\Users\lcj\neptune-build` : `build_nep.cmd` + `inst_nep.ps1` (plugin Neptune, dossier `build`), `build_diapason.cmd` + `inst_diapason.ps1`, `build_sa_asio.cmd` (Standalone avec ASIO, dossier `build-sa`, kit Steinberg GPLv3 dans `asiosdk`, **hors dépôt**). Standalone Windows livré dans `C:\Users\lcj\neptune-standalone\Neptune.exe` (+ l'exemple de Façade).
- Plugins installés : Linux `~/.clap/`, Windows `C:\Program Files\Common Files\CLAP\Neptune-Win\`.
- Vérification à l'écran sans Reaper : Standalone (`build-local/adapter-juce/NeptuneApp_artefacts/Release/Standalone/Neptune`, ou `Neptune_artefacts/.../Neptune_Standalone`, `Diapason_artefacts/.../Diapason`) lancé sous Xvfb, capture `xwd -root`, clics et frappes par python-xlib (XTEST). Les scripts de mes essais (`shot2.sh`, `runner.py`) sont dans le dossier temporaire de la session et seront perdus : les refaire (Xvfb :97 1700x1200x24, `size L H` redimensionne la fenêtre, clic gauche et droit, glisser, appui maintenu, saisie). Toujours comparer `settings.json` avant / après.

## Ce qui a été fait cette session (tout commité en local, installé Linux et Windows sauf la dernière étape)
1. **Scènes** (ex-combinaisons) : modèles d'enregistrement (« Tout Neptune » par défaut ; anciens projets restent en « Combinaison d'orgue »), annulation = valeurs par défaut, paramètres des inserts inclus (type de module vérifié au rappel, inserts ignorés signalés dans la barre du bas), glissement réglable au rappel (continu, 0 à 2000 ms), stepper (écran Scènes et barre du bas), **32 banques de 64 scènes**, changement de programme MIDI 0-63 scène / 64-95 banque / 96-127 scène de piste, grille de pas jusqu'à 256 pas.
2. **Barre du bas de Neptune** en icônes (profil, cadenas, annuler / rétablir) ; boîtes JUCE au style NOVA (`nova::DialogLook`).
3. **Diapason** : page MIDI (envoi vers la sortie MIDI de l'hôte), page OUTILS (accordeur qui écoute par YIN, 21 accordages, note de référence, métronome), batterie en grille régulière avec pas en doubles-croches / croches, version en bas à droite (socle NOVA).
4. **Neptune Standalone** : cible à part `NeptuneApp` (option `NEPTUNE_BUILD_STANDALONE`), ASIO (GPLv3) + WASAPI, sortie MIDI vers appareil, une seule sortie large de 64 canaux pour choisir toutes les sorties de la carte (UMC de Jean : fonctionne).
5. **Façade** : nouveau widget **grille de scènes** (4 à 8 colonnes et lignes, nom + deux lignes de texte par scène, barre de banque à appui long, SET facultatif, couleur et tempérament, clic droit nom et texte, taille du nom et des textes), exemple `docs/exemple-facade/Concert-scenes.neptunefacade` (script `generer-concert.py`, à charger dans la Façade **globale**), barre d'onglets à gauche en jeu sur écran large et repliable.

## Travail en cours (non terminé au moment de ce prompt)
Dernière demande de Jean, déjà codée, **non commitée, non vérifiée à l'écran, non installée** :
- bouton de dépliage de la barre : petit carré collé en haut à gauche de la Façade ;
- dans les cases de la grille : le nom commence tout en haut (à la hauteur du numéro) ; texte réduit automatiquement s'il ne tient pas (`drawAutoFit`) ;
- tailles par défaut : nom 150 %, textes 250 % ; plages 50-400 % et 50-500 % ; réglage par un curseur en direct (menu « Grille... » → « Taille du nom... », « Taille des deux lignes de texte... », un seul pas d'annulation par geste via `liveMergeKey_`) ;
- flèches de banque : barre qui se remplit pendant l'appui long (500 ms) et mention « appui long sur les flèches pour changer de banque ».
Compilation Linux lancée (`build-local`, cibles `Neptune_CLAP NeptuneApp_Standalone`). **À faire** : vérifier le résultat (Standalone sous Xvfb : grille avec une scène nommée, curseurs de taille, bouton replié), lancer les 930 tests, commiter, installer Linux (Reaper fermé), envoyer les sources à Windows, `build_nep.cmd` puis `inst_nep.ps1` et `build_sa_asio.cmd` puis copie de `Neptune.exe` (Reaper et `Neptune.exe` fermés), mettre à jour `echanges/etat-versions.md` et `docs/plan-scenes.md`.

## Points ouverts (par ordre d'utilité probable)
- Retours de Jean dans Reaper sur : la grille de scènes et l'exemple Concert (Windows), l'accordeur sur une vraie guitare ou basse, le métronome en mode hôte, le changement de programme MIDI.
- Standalone : vrai menu Ouvrir / Enregistrer un projet (aujourd'hui seulement « Save / Load a saved state » du menu Options de JUCE), bandeau jaune « Audio input is muted », sens du profil Live / Studio sans hôte, installateur, signature de code (SmartScreen), compilation macOS par GitHub Actions (**ne rien pousser sans accord** ; non notarisé : l'utilisateur autorise l'app dans Réglages Système).
- Grille de scènes : possibilité de plusieurs grilles, couleur par banque, taille de la grille au-delà de 8 x 8 (limite : 64 scènes par banque).
- Assistant d'import d'orgue (`ImportWizard`) : jamais vu à l'écran, utilise déjà les jetons de couleur.
- Poids des scènes « Tout Neptune » (60-70 ko chacune) : Jean a dit que ce n'était pas la peine.
- Autres plugins : le numéro de version passe dans le pied (socle NOVA) à leur prochaine compilation.
