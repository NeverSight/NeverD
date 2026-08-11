**Langues**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Reconstruction des exceptions Windows

[← Index de la documentation](README.fr.md)

NeverD transporte les informations d’exception Windows fondées sur des tables
pendant le chargement, le lift, la décompilation et la réécriture binaire. Ces
metadata font partie du contrat exécutable d’une fonction : une réécriture est
refusée si NeverD ne peut pas prouver la cohérence du code généré, des records
runtime-function, des tables de langage et des tables de protection.

Trois niveaux de support sont distingués :

- **Analyse** : la représentation native devient un record normalisé et vérifié,
  exposé à la pipeline IR.
- **Décompilation** : les régions protégées réductibles deviennent des nœuds
  d’exception HighIR ; les autres gardent des annotations natives déterministes.
- **Reconstruction native** : le mode patch peut faire produire à LLVM un contrat
  d’exception de remplacement complet et l’installer dans le PE final.

Le support d’analyse n’implique pas le support de reconstruction native.

## Matrice de support

| Forme native | Lift et analyse | Sortie de haut niveau | Mode patch |
|--------------|-----------------|------------------------|------------|
| unwind x64 v1/v2 | Records, opérations, chaînes, données de handler et provenance entièrement vérifiés | Résumé frame/unwind et régions de langage structurées si possible | Pris en charge pour les records primaires complets ; les `.pdata`/`.xdata` générées remplacent la fermeture obsolète |
| unwind x64 v3/APX | Payload v3, épilogues et comptage des opérations dédiés | Annotations v3 explicites | Analyse seulement ; toute fonction touchée est refusée |
| unwind ARM32/ARM64 packed | Plages, champs packed, identité primary/fragment | Résumé frame/unwind | Seulement pour un record primaire complet sans langage ni fragment adressable séparément |
| unwind ARM32/ARM64 unpacked | Header/étendue xdata vérifiés, association du handler et fragments | Résumé frame/unwind | Seulement pour un record primaire complet sans langage ni fragment adressable séparément |
| `__C_specific_handler` | Scopes, filtres, cibles finally, handlers et continuations | `__try`/`__except`/`__finally` pour les régions réductibles, annotation sinon | Reconstruction x64 native des graphes de scopes complets et représentables |
| `__CxxFrameHandler3` | Unwind map, try map, catches, offsets objet/frame, continuations et IP-to-state map | Intervalles réductibles en C++ HighIR avec annotations de type compatibles C | Reconstruction x64 du sous-ensemble volontairement étroit et verifier-clean décrit plus bas |
| `__CxxFrameHandler4` | Décodage variable borné vers le graphe C++ commun, actions et offsets inclus | Même graphe HighIR avec provenance FH4 | Analyse seulement ; fonction touchée refusée |
| `__GSHandlerCheck_SEH/EH/EH4` | Personality enveloppée et provenance GS cookie vérifiée | Graphe du langage de base et annotation wrapper | Analyse seulement ; refus sans downgrade |
| EH x86 par chaîne d’enregistrement | Distinct de l’EH tabulaire | Annotation de forme non prise en charge | Non reconstruit |

Un record malformé n’est jamais considéré comme complet. Un décodage partiel
reste consultable mais n’autorise pas la génération native. Si un header xdata
ARM prouve encore une plage de fragment exécutable bornée malgré un corps unwind
tronqué, la plage reste disponible pour le désassemblage, mais le record est
marqué malformed et ne devient pas patchable.

## Modèle normalisé

`ExceptionInfo` appartient à `BinaryImage`. Chaque `ExceptionFunction` contient :

- une plage de code semi-ouverte vérifiée ;
- une identité primary, chained ou fragment ;
- l’encodage unwind natif et sa provenance runtime/unwind exacte ;
- les opérations et épilogues normalisés, avec les operands opaques non compris ;
- l’identité exacte de la personality et ses données de handler ;
- les scopes SEH, maps d’état C++ et données GS cookie éventuels ;
- un état `Complete`, `Partial` ou `Malformed` et des diagnostics déterministes.

Le loader n’expose aucun pointeur brut. Les RVA natives servent au diagnostic et
au remplacement ; les consommateurs IR n’utilisent que des VA et plages validées.

L’index global accepte le chevauchement des records chained/fragment et retourne
la fonction la plus spécifique. Répertoire, plage, pointeur, compteur, transition,
entier compressé ou chaîne corrompu, ainsi que l’épuisement du budget de décodage,
abaissent l’état d’analyse concerné.

Les limites s’appliquent par table et au graphe complet d’une fonction : la
réutilisation d’une handler map par plusieurs try entries ne multiplie pas le
travail au-delà du budget global. Les records FH3 partageant `FuncInfo` et la
personality forment un groupe borné, ce qui autorise les catch funclets du parent
sans accepter les adresses de fonctions sans rapport.

## Contrat IR

Les metadata d’exception traversent toutes les représentations sans modifier le
sens du CFG ordinaire :

- LowIR coupe les blocs aux limites des régions, transitions d’état, filtres,
  handlers, actions de cleanup et continuations.
- Les successeurs/prédécesseurs exceptionnels sont séparés des arêtes ordinaires.
- MedIR conserve le descripteur normalisé et les arêtes exceptionnelles stables.
- HighIR distingue `SEHTry` de `CxxTry` et conserve VA, type descriptor,
  adjectives, offsets d’objet/frame, actions, états et continuations.

Le structurer HighIR est conservateur : il ne déplace qu’une tranche contiguë
entièrement comprise dans une région complète et traite l’imbrication de
l’intérieur vers l’extérieur. Régions croisées, graphes partiels, limites
ambiguës et funclets hors ligne restent dans le contrôle de flux original.

Le backend C émet la syntaxe MSVC SEH pour une région SEH réductible à clause
unique. HighC étant un backend C, catches C++ et états de cleanup deviennent des
commentaires C déterministes, sans prétendre produire du C++ compilable.

## Schéma des metadata LLVM

Chaque fonction d’exception analysée reçoit des metadata sans perte, même sans
lowering WinEH natif :

- attachment : `neverd.windows.eh` ;
- marqueur natif : `neverd.windows.eh.native` ;
- table de module : `neverd.windows.eh.functions` ;
- version du schéma : `3`.

Le record fixe conserve état, encodage, plage, RVA runtime/unwind, type de record,
chaîne, mot packed, frame, noms de personality, handler, octets unwind, opérations
et épilogues, scopes SEH, maps C++, données GS, diagnostics et droit de
régénération. Le patch exige la version exacte et une plage identique à l’image.

Le lowering SEH x64 natif utilise LLVM WinEH et n’émet un flux
`invoke`/funclet verifier-clean que si le graphe entier est représentable. FH3
exige en plus :

- x64 COFF, unwind v1/v2, metadata complètes et graphe FH3 synchrone valide ;
- aucune sémantique `noexcept`, asynchronous, separated-funclet, GS, FH4 ou flag inconnue ;
- intervalles imbriqués ou disjoints, jamais croisés ;
- aucun destructeur, unwind action, objet catch ni dépendance au parent frame ;
- un handler dans un bloc ordinaire sans prédécesseur ni call ;
- un LLVM `invoke` pour toute opération protégée susceptible d’un unwind.

Sinon, l’IR reste analysable et sans perte, mais le remplacement natif est
refusé. Entry point PE, callbacks TLS et racines CRT restent des frontières à
préserver plutôt que des candidats ordinaires à la réécriture ABI.

## Transaction de patch

Une réécriture prise en charge forme une transaction PE unique :

1. Valider chaque fonction touchée contre le graphe chargé et les metadata LLVM.
2. Compiler en conservant identité, alignement et traits des sections ainsi que
   les références sémantiques. Externaliser la personality Windows locale pour
   lier xdata au handler exécutable d’origine prouvé.
3. Garder les runtime functions intactes et retirer toute la fermeture native
   remplacée, chaînes comprises.
4. Reloger code/xdata, fusionner et trier pdata, refuser les chevauchements,
   prouver la classe de personality et installer un seul répertoire d’exceptions.
5. Préserver le mode CFG, résoudre `.gfids`/`.gehcont`, fusionner Guard CF et
   Guard EH continuation, puis mettre à jour load-config. Un helper non résolu
   annule tout. CFW, return-flow guard, retpolines et XFG restent analysis-only.
6. Réanalyser l’image d’octets terminée avant l’écriture.

L’extension du fork LLVM reste générique : le writer final-image conserve les
traits de section et références symboliques. PE, tables MSVC, politiques,
fusion, load-config et validation finale restent dans NeverD.

Les entrées Guard CF/EH continuation d’origine sont conservées, car leurs
trampolines restent des cibles indirectes valides. Les cibles générées doivent
être dans le code émis et toutes les tables strictement triées par RVA.

## Validation de l’image finale

Un PE patché est refusé sauf si :

- LLVM accepte les octets comme COFF et machine, classe, sections, répertoires,
  image base et étendue concordent ;
- toutes les étendues raw/virtuelles sont bornées et non chevauchantes ;
- le répertoire d’exceptions est présent dans le fichier et dans l’image ;
- les runtime functions sont triées, non vides, non chevauchantes et exécutables ;
- RVA unwind, headers, versions, flags, handlers et chaînes x64 sont valides ;
- imports, exports et symboles COFF finaux permettent de réanalyser SEH/FH3 ;
- records ARM et xdata décrivent une version/plage prise en charge ;
- les champs Guard CF/EH existent lorsque les flags les annoncent ;
- pointeurs, compteurs et strides restent dans le fichier/l’image, avec des
  entrées strictement triées vers des cibles exécutables.

Tout échec annule le patch ; aucune image best-effort n’est écrite.

## Vérification ciblée

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture x64 protégée est assemblée et liée avec `/guard:cf` et
`/guard:ehcont`. Elle vérifie scopes SEH, Guard, HighC, patch, rechargement,
comptage, tri et cibles exécutables. La fixture FH3 liée vérifie séparément les
tables fixes, annotations, personality, graphe try/catch et map IP-to-state.
Exécuter aussi les cas ARM lors d’une modification du parser.

## Extension du support natif

Toute nouvelle forme native doit ajouter dans le même changement :

- parser complet et borné, avec invariants du modèle ;
- round-trip HighIR et metadata LLVM ;
- IR natif verifier-clean pour chaque nouveau graphe accepté ;
- conservation des sections et références nécessaires ;
- fixture PE liée pour architecture/personality/version exacte ;
- validation exception-directory, load-config et image finale ;
- tests de refus explicites des formes non prises en charge les plus proches.

La capacité de décoder un record ne suffit jamais à élargir l’allow-list. Le
critère est la préservation du comportement d’exception dans l’image liée finale.
