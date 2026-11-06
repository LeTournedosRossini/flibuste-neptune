# Prompt de reprise — Flibuste Suite / Neptune (2026-11-07)

Tu reprends le travail sur la Flibuste Suite (Neptune : sampler / orgue / synthé / console de scènes en CLAP et Standalone ; plugins NOVA : Digue, Diapason, Mirador, AnalogSynth, FM6Op, Orgamon, Moussaillon, Fournaise, Carte, Arpège, Essai Carte, et le nouveau **Trident**). Jean est musicien et enseignant, il dicte (erreurs de reconnaissance vocale : « Scorser » / « Corsair » = Scorsaire) : réponds en français, simplement, en disant ce qui est fait et ce qui est vérifié.

**Commence par lire** : ce fichier, `docs/prompt-reprise-2026-11-06.md` (règles qui ne bougent pas, scripts Windows, vérification à l'écran), `docs/exemples-arpege/LISEZMOI.md`, et les mémoires de `~/.claude/projects/-home-jean-Bureau-FlibusteSuite/memory/` (notamment `project-reste-a-faire-2026-11-06.md` et `project-trident-splitter.md`).

## Règles (rappel court)
- Pousser sur GitHub **seulement si Jean le demande** (demander à chaque fois) ; commits terminés par `[skip ci]` et `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. La CI ne se lance que sur son accord (`gh workflow run build.yml --ref main`).
- Après chaque bonne compilation : installer Linux et Windows, Reaper fermé, ancien fichier sauvegardé dans `~/neptune-anciens/<horodatage>/`, SHA256 comparé. Compilations par `~/.local/bin/claude-build linux|windows ...`.
- Messages aux autres conversations dans `echanges/messages/` (Jean relaie) ; tu tiens `echanges/etat-versions.md`. Les menus utilisent le LookAndFeel nova.

## Ce qui est fait (6 novembre)
- Conduite de voix partout, grattage, accords d'approche, coups étouffés, bibliothèque de motifs, motifs appelés par Carte dans Orgamon, AnalogSynth et FM6Op (pilote partagé `adapter-juce/src/CarteDriver.h`) ; canaux par défaut haut 1 / bas 2 / pédale 3. Jean a essayé : « tout fonctionne bien ».
- **Trident** : séparateur à trois chemins (parallèle ou multibande 2 / 3 bandes, pentes 12 / 24 / 48), quatre effets de la suite par chemin, 32 macros, latences alignées. Jean a essayé dans Reaper : « top ». Pas de presets, pas de sidechain, pas de traduction.
- Poussé : jusqu'à `ac4d055`. Un commit local non poussé corrige les deux échecs de la première CI (voir ci-dessous).

## CI du 6 novembre (run 42220863195, lancée à la main)
- **macOS** : les trois tests « de bout en bout » de Carte échouaient (`writer.open` renvoie Error) : le nom de la zone partagée dépassait les 31 caractères permis par macOS. Corrigé dans `core/src/SharedChordMap.cpp` (empreinte du nom, macOS seulement). Non vérifié sur macOS.
- **Windows** : 108 tests « échoués » parce que `ctest` ne retrouve pas les noms de tests accentués (« No test cases matched »). Corrigé dans `.github/workflows/build.yml` : sous Windows, le programme de tests est lancé directement. Non vérifié.
- Linux : en cours quand ce prompt a été écrit ; la compilation des plugins (dont Trident) sur macOS et Windows n'a pas encore été vue, car les tests échouaient avant (l'étape de compilation des plugins vient après dans le même travail).
- À faire d'abord : lire le résultat final, puis, avec l'accord de Jean, pousser le correctif et relancer la CI.

## À faire (liste complète dans la mémoire `project-reste-a-faire-2026-11-06.md`)
Doigtés de guitare pour le grattage (demander `fret-shapes.json` à Scorsaire) ; le sampler Neptune lit la Carte ; traduction de Carte et de Trident ; presets et sidechain de Trident ; boutons d'approche dans Arpège ; constructeur d'instrument piano ; essai de trois Arpège sur une Carte ; vérification Windows à l'écran.
