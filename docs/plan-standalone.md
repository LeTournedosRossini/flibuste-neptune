# Neptune Standalone (application autonome) — 2026-11-02

## Principe
Une cible **à part** (`NeptuneApp`, produit `Neptune`), désactivée par défaut (`-DNEPTUNE_BUILD_STANDALONE=ON`). Elle reprend la liste des sources de la cible Neptune (le .clap) et construit sa propre copie ; aucune source ni aucun réglage de compilation des plugins n'est modifié (le diff de `adapter-juce/CMakeLists.txt` n'est qu'un bloc ajouté à la fin). L'exécutable est la cible `NeptuneApp_Standalone`.

## Construire
- Linux : `cmake -S flibuste-neptune -B build-local -DNEPTUNE_BUILD_STANDALONE=ON` puis `cmake --build build-local --target NeptuneApp_Standalone`. Vérifié : l'application s'ouvre (Xvfb).
- Windows (machine `neptune-win`) : scripts `build_sa.cmd` (WASAPI) et `build_sa_asio.cmd` (ASIO) dans `C:\Users\lcj\neptune-build`, dossier de construction `build-sa` (séparé de `build`, celui des plugins). Résultat copié dans `C:\Users\lcj\neptune-standalone\` (hors du dossier CLAP).

## ASIO
- Kit Steinberg ASIO SDK 2.3.4, double licence (propriétaire / **GPLv3**), téléchargé sur https://www.steinberg.net/asiosdk (page https://www.steinberg.net/developers/asiosdk-open/) avec l'accord de Jean le 2026-11-02. Neptune est sous AGPLv3 : l'option GPLv3 est celle utilisée.
- Le kit **n'est pas dans le dépôt** (sa licence propriétaire interdit de le redistribuer ; sous GPLv3 on pourrait, mais ce n'est pas nécessaire) : dossier `C:\Users\lcj\neptune-build\asiosdk` (sous-dossier `common`), passé par `-DNEPTUNE_ASIO_SDK_DIR=...`.
- Nom et logo « ASIO » : marques de Steinberg ; pas de logo ; le nom seulement dans « À propos » ou la documentation (« compatible ASIO »), jamais dans un nom de produit.
- JUCE 8.0.11 : les en-têtes sont lus dans le dossier du kit (`JUCE_ASIO=1`).

## Sorties multiples (2026-11-02)
Le Standalone de JUCE limite les sorties choisissables (fenêtre « Options ») à la taille de la première sortie du processeur (2 canaux : une seule paire à la fois). Dans l'application seulement (`NEPTUNE_STANDALONE_APP`), Neptune déclare donc **une seule sortie de 64 canaux** au lieu de 32 sorties stéréo ; le moteur travaille par numéro de canal, le routage des pistes vers « Out N » est inchangé. Les sorties de Neptune se rangent sur les sorties **actives** de la carte son, dans l'ordre : Out 1 sur la première paire cochée, Out 2 sur la deuxième... (pour que Out 3 sorte sur les canaux 5-6, cocher aussi 1-2 et 3-4). Vu sur le Standalone Linux (la fenêtre s'ouvre) ; pas encore essayé avec une interface multi-sorties.

## Projets
Le menu « Options » de JUCE offre déjà « Save current state... », « Load a saved state... » et « Reset to default state » : un projet autonome existe donc déjà (fichier d'état). Un vrai menu Fichier reste à faire si besoin.

## Reste à faire
Sortie MIDI vers un appareil, menu Ouvrir / Enregistrer un projet, sorties multiples vers la carte son, bandeau « Audio input is muted », texte du profil Live / Studio dans ce contexte, installateur, signature de code (SmartScreen), macOS (compilation par GitHub Actions, non fait ; non notarisé : l'utilisateur doit l'autoriser dans Réglages Système > Confidentialité et sécurité).
