**Langues**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](CONTRIBUTING.md) | [Deutsch](../de/CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# Contribuer à NeverD

NeverD est un projet d’analyse binaire qui donne la priorité à la sémantique.
Une contribution utile reste ciblée, fait échouer explicitement les
comportements non pris en charge et inclut le plus petit test qui prouve le
contrat modifié.

Conservez les mentions de droit d’auteur, de licence et de source lorsque vous
intégrez du code existant, y compris du code copié ou adapté avec des outils
d’IA/LLM. Suivez le [guide d’attribution](ATTRIBUTION.md), indiquez la source
et la révision d’origine des éléments réutilisés, et vérifiez leur licence
avant de soumettre votre contribution. Conservez les mentions tierces sous
leurs licences d’origine.

Avant toute modification, lisez le [guide d’architecture](architecture.md).
Consultez le [guide des tests](testing.md) pour choisir une suite et la
[feuille de route](roadmap.md) pour les travaux produit planifiés.

<!-- i18n-section: prerequisites -->
## Prérequis

- Git avec prise en charge des sous-modules récursifs
- CMake 3.20 ou version ultérieure
- Ninja
- Un compilateur C++20
- CPython 3.10+ et fichiers de développement pour l’intégration, si les plugins Python sont activés
- Clang et LLD (`ld.lld` et `lld-link`) pour l’ensemble complet de fixtures
  multi-cibles

Les sous-modules récursifs fournissent les forks LLVM et Capstone de NeverD,
Unicorn et les données de signatures. Ne les remplacez pas par des révisions
système arbitraires lors de la validation d’une modification.

<!-- i18n-section: clone -->
## Cloner et initialiser

Le développement est intégré dans `dev`, qui est également la branche par
défaut du dépôt. Clonez-la avec tous les sous-modules :

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

Dans un clone existant, synchronisez les sous-modules avant la première
compilation et après tout commit qui modifie leurs révisions enregistrées :

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## Choisir un profil de compilation

| Profil | Utilisation | Comportement important |
|--------|-------------|------------------------|
| Release | Développement normal, tests complets, benchmarks de décodage/lifting | Optimisé ; débit représentatif |
| RelWithDebInfo | Profilage ou débogage des chemins critiques optimisés | Optimisé avec symboles de débogage |
| Debug | Assertions, exécution pas à pas, vérification locale | Non optimisé ; benchmarks de décodage volontairement bien plus lents |

Utilisez Release sauf si la tâche exige spécifiquement le comportement Debug :

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

Par défaut, la compilation construit `third_party/llvm-project` comme
dépendance intégrée. La première compilation prend généralement 30 à 60
minutes ; les suivantes sont incrémentales. `CMakePresets.json` définit aussi
les presets de configuration/compilation `release`, `relwithdebinfo` et
`debug`, mais les répertoires explicites ci-dessus rendent visible l’activation
des tests.

Pour déboguer au niveau source, utilisez un répertoire séparé au lieu de
reconfigurer l’arborescence Release :

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Ne publiez jamais un débit de décodage ou de lifting mesuré avec une
compilation Debug. Utilisez Release pour les benchmarks, ou RelWithDebInfo si
le profilage nécessite des symboles.

<!-- i18n-section: allocator -->
### Allocateur du processus

Les builds Linux, macOS et Windows MSVC/clang-cl activent `NEVERD_ENABLE_MIMALLOC`
par défaut. Les outils, workers d’analyse et tests créés par l’utilitaire CMake commun
utilisent la branche par défaut du fork mimalloc de NeverSight, suivant le modèle hôte
de NeverC. Unix l’intègre une fois par exécutable. Windows utilise une DLL mimalloc et
`mimalloc-redirect*.dll` de même architecture pour les allocations du CRT dynamique,
y compris libneverd et les plugins natifs. Les deux DLL sont placées près des exécutables
et installées dans `bin` : distribuez-les ensemble. Windows exige `/MD` ou `/MDd` ;
désactivez mimalloc pour un CRT statique. Le SDK n’impose aucun allocateur à l’hôte.
Sur Windows ARM64, utilisez un build ARM64 natif : une DLL x64 ne peut pas rediriger le
CRT ARM64EC de l’émulation x64. MinGW conserve l’allocateur système. Voir les
[exigences Windows en amont](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md).

Utilisez `-DNEVERD_ENABLE_MIMALLOC=OFF` pour la référence système ou un allocateur externe.
L’ancienne option `NEVERD_WORKER_MIMALLOC=OFF` ne désactive que le worker. CMake supprime
mimalloc avec les sanitizers address, thread, memory et leak, y compris les indicateurs
combinés ou propres à une configuration, de compilation ou d’édition de liens. UBSan seul
reste compatible. Avec des indicateurs de sanitizer propres à une cible, définissez aussi OFF.
Sous Windows, `MIMALLOC_DISABLE_REDIRECT=1` permet une comparaison à l’exécution sans
recompiler ; `MIMALLOC_VERBOSE=1` signale l’activation.

L’entrée prise en charge, `python scripts/build.py BUILD_DIR`, actualise les cinq forks
NeverSight depuis leurs branches distantes par défaut avant chaque build. Ce sont des
sous-modules de `third_party`, comme Capstone et Unicorn. Le script résout les vrais noms
de branches, actualise ces cinq sous-modules et exporte leurs sources commitées vers
`BUILD_DIR/third_party/<library>/<commit>/src`. Du travail local non commité ou divergent
dans ces sous-modules arrête le build sans écrasement, tout comme un échec réseau.
Le répertoire est verrouillé pendant actualisation, reconfiguration et compilation ; le
snapshot reste fixe jusqu’au prochain appel. Les builds incrémentaux adoptent aussi les nouveaux commits.

`neverd-dependency-inputs.json` consigne les entrées de configuration, commits et SHA-256
des exports, et accompagne les produits installés ; il ne certifie pas un binaire.
`neverd-build-receipt.json` consigne succès/échec uniquement pour les cibles et la configuration
demandées. La CI conserve les deux. L’installation refuse builds échoués, entrées modifiées
et configurations non compilées ; la CI produit compile toutes les cibles installées avant
de tester l’installation. Comparez les commits avant de qualifier ensemble plusieurs plateformes,
car la branche principale peut évoluer entre exécutions. `cmake --build` seul vérifie la fraîcheur
et refuse les snapshots périmés ; utilisez l’entrée Python pour les actualiser. Sa régénération
automatique conserve le même snapshot. Les surcharges `FETCHCONTENT_SOURCE_DIR_NEVERD_*`
sont réservées aux fixtures locales isolées et refusées par l’entrée principale. Les générateurs
multi-configuration exigent `--config`. Compilez par l’entrée, puis lancez tests et
`cmake --install` séparément ; la cible directe `install` est refusée avant compilation.

Voir la [validation de l’allocateur](../testing.md#allocator-validation) pour les tests de fumée et comparaisons.

<!-- i18n-section: acceleration -->
### Bibliothèques d’accélération

Les builds normaux activent le balayage d’octets Highway, la validation UTF-8 simdutf,
l’analyse du transport simdjson et l’ordonnancement oneTBB, en conservant l’allocateur du
processus. JSON conserve le DOM et la sérialisation nlohmann : les trames valides composées
d’entiers sont copiées depuis la bande SIMD ; décimaux et zéro négatif gardent la conversion
d’origine. Les entrées hors domaine simdjson suivent l’admission d’origine. Le texte externe
invalide conserve ses échappements fidèles aux octets. Ces bibliothèques et mimalloc proviennent
des forks NeverSight de `third_party/forks.json`, corrigés sur leurs branches par défaut.
Le build copie les licences courantes et consigne chaque commit exact ; ces cinq dépendances ne sont pas figées sur d’anciennes versions.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON` et
`NEVERD_ENABLE_TBB` valent `ON` par défaut, chacun pouvant passer à `OFF` pour comparaison.
oneTBB conserve ses workers entre lots, vérifie leurs piles de 8 MiB et ne compile ni ne charge
`tbbmalloc` ou son proxy. Les appelants à grande pile réutilisent leur marge, les autres
reçoivent un coordinateur natif. Les hôtes qui forkent après la première utilisation ou
déchargent dynamiquement le SDK doivent utiliser les threads natifs (`NEVERD_ENABLE_TBB=OFF`) :
ces cycles de vie ne sont pas pris en charge par l’ordonnanceur persistant. oneTBB est lié
statiquement au noyau partagé, et ses symboles restent privés à ce noyau. Les appels aux
composants depuis la CLI et les requêtes au worker de la GUI utilisent le même ordonnanceur ;
aucune DLL ni bibliothèque partagée TBB n’est distribuée. Les cibles liées à `neverd_shared`
sélectionnent automatiquement l’ordonnanceur partagé. Les clients qui accèdent au SDK par
une autre cible et lient également des archives de composants doivent appliquer
`neverd_use_shared_task_scheduler` à leur cible finale. Les outils de composants autonomes
conservent leur propre environnement d’exécution.
Cette intégration restreinte évite plusieurs ordonnanceurs NeverD dans un même processus ;
le projet amont considère toujours les [compilations statiques de oneTBB](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
comme non prises en charge et déconseille leur usage général.

Voir la [validation des accélérations](../testing.md#acceleration-libraries) pour les tests natifs, comparaisons et limites de compatibilité.

<!-- i18n-section: prebuilt-llvm -->
### LLVM précompilé sur macOS

Les contributeurs sur Apple Silicon peuvent éviter de compiler localement le
fork LLVM :

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake télécharge le paquet de version configuré par le dépôt, vérifie sa somme
SHA-256 et réutilise ensuite le cache utilisateur extrait. Le canal précompilé
prend uniquement en charge macOS arm64. Les Mac Intel et les builds universels
doivent employer la compilation LLVM locale par défaut. Les options avancées,
comme `NEVERD_LLVM_PREBUILT_TAG`, l’URL du miroir, le répertoire de cache et une
somme explicite, sont documentées dans `cmake/NeverDLLVMPrebuilt.cmake`.

Les corrections LLVM appartiennent au [fork LLVM NeverSight](https://github.com/NeverSight/llvm-project).
Commitez-les et fusionnez-les dans ce fork, puis mettez à jour la révision du sous-module LLVM de NeverD.
La compilation intégrée compile directement ces sources ; n’ajoutez ni fichiers de correctifs LLVM ni copies modifiées dans le dossier de compilation de NeverD.
Les paquets précompilés doivent être reconstruits depuis le fork corrigé pour valider le même comportement.

<!-- i18n-section: workflow -->
## Flux des branches et pull requests

Partez d’un `dev` à jour et créez une branche thématique ciblée :

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Ouvrez les pull requests vers `dev`, et non vers une branche de publication
supposée. Gardez les commits faciles à relire : un objectif cohérent, aucun
artefact de compilation généré, aucun reformatage sans rapport et aucune
révision de sous-module modifiée sauf si elle fait partie de la proposition.

<!-- i18n-section: code-style -->
## Style du code

C/C++ suivent les [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html),
avec [`.clang-format`](../../.clang-format) pour le format et
[`.editorconfig`](../../.editorconfig) pour l’encodage, les fins de ligne et espaces.
La version C++ vient de `CMakeLists.txt`, pas du guide amont. Formatez les fichiers modifiés :

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

La CI utilise la version fixée dans [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py) ;
reprenez-la pour reproduire ses résultats. Le formatage ne vérifie ni la responsabilité des
en-têtes ni les littéraux de politique. Pas de reformatage global pour une correction locale.
Suivez le nommage et découpage voisins ; gardez le comportement plateforme aux frontières
loader/lifter/backend et les types C++ internes hors du SDK C pur.

<!-- i18n-section: headers -->
### Organisation des fichiers et en-têtes

- Les nouveaux fichiers C++ commencent par la bannière LLVM nom/objet, `NeverD Decompiler`
  et Doxygen `\file`, comme `lib/debug/PDBLoader.cpp`. Préservez les mentions de licence et copyright.
- Les en-têtes autonomes utilisent `.h` et incluent explicitement leurs dépendances.
  L’implémentation inclut son interface en premier. `.clang-format` classe ensuite les en-têtes
  privés voisins, publics `neverd/`, LLVM/Capstone entre guillemets, puis entre chevrons.
  Ne dépendez ni d’un ordre accidentel ni d’inclusions transitives.
- Les nouveaux en-têtes ont une garde dérivée du chemin `NEVERD_..._H`, par exemple
  `NEVERD_SDK_LIMITS_H` pour `neverd/sdk/Limits.h`. Incluez assez de répertoires privés/outils
  pour éviter les collisions ; commentez le nom à `#endif`. Appliquez ce modèle lors d’une
  harmonisation explicite d’un ancien en-tête, sans toucher aux fichiers étrangers au changement.
- Placez les interfaces partagées dans `include/neverd/<component>/`, les en-têtes privés près
  des sources dans `lib/` ou `tools/`. Leur emplacement ne garantit pas une API C SDK stable.
  Dépendances acycliques ; modèles/lecteurs centraux indépendants des sessions et transports.
  Algorithmes dans leur composant, sans dupliquer la sémantique CLI/SDK/worker. Voir [architecture](architecture.md).
- Utilisez l’espace de noms propriétaire et des noms explicites : nouveaux types/champs
  `UpperCamelCase`, fonctions `lowerCamelCase`, constantes de politique `kUpperCamelCase`.
  Respectez les interfaces existantes, contrats C/Python et conventions STL ; aucun
  `using namespace` portant sur tout un espace de noms dans les en-têtes.
- Les tables volontairement multi-incluses gardent `.def`/`.inc` et documentent leur contrat
  de macros ; ce ne sont pas des interfaces `.h` autonomes et elles n’ont pas de garde empêchant
  les expansions répétées. Modifiez les en-têtes générés via leur producteur.

Exemple d’en-tête de politique autonome :

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Responsabilité des limites, valeurs par défaut et configurations

Utilisez `ResourceLimits.h` dans les répertoires d’outils ajoutés directement au chemin de recherche des en-têtes. Sur un système de fichiers insensible à la casse, un fichier local `Limits.h` peut masquer le fichier système `<limits.h>`.

Chaque politique a une définition unique dans l’en-tête de limites/configuration de son
composant, même avec un seul appelant : entrées/sorties, récursion/graphes, budgets de travail
et d’itération, seuils heuristiques, cache, pagination/lots, reprises et délais. La règle vaut
pour `constexpr`, macros, comparaisons littérales, `min`/`clamp`, initialisations et paramètres par défaut.

Choisissez d’abord l’autorité existante :

| Responsable | Définition |
| --- | --- |
| Cœur/pipeline partagé | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) ou définition ciblée existante |
| Autre composant | En-tête/table existant, sinon `Limits.h` ciblé dans la couche propriétaire |
| CLI, worker ou GUI seulement | `ResourceLimits.h` ou `Config.h` local à l’outil, sans dépendance du cœur |
| Options de l’appelant | Modèle propriétaire `Config.h`, `*Options.h` ou `*Configuration.h` |

Gardez les en-têtes légers, groupés par objet. Réutilisez `*Limits.def` ou les tables de
configuration et exposez leur vue C++ dans un en-tête autonome. Pas de définitions concurrentes
dans `Limits.h`, `Constants.h`, `Config.h`, de politique centrale dans SDK/outils, ni de toutes les politiques sans rapport dans le `Limits.h` racine.

Les scalaires C++ sont des `inline constexpr` typés dans l’espace existant (`neverd::limits`,
`neverd::sdk`). Nommez objet et unité : `kMaxSourceBytes`, `kDefaultPageRows`,
`kRetryDelayMilliseconds` ; documentez zéro/sentinelles et bornes inhabituelles. Distinguez
défaut et maximum dur s’ils diffèrent. Un modèle d’options peut posséder son défaut ; les
consommateurs utilisent modèle, fabrique ou constante commune. Dérivez les valeurs liées sans répéter de littéraux.

Adaptateurs, persistance, CLI/GUI/worker et Python/MCP réutilisent l’autorité, y compris
maxima de schéma et diagnostics chiffrés. Dérivez les contrats multilangues d’une table ou
liaison générée ; sinon ajoutez un contrôle de dérive. Préservez les en-têtes C purs.
Une limite d’aperçu/page plus stricte a son propre nom et propriétaire ; égalité numérique ne signifie pas politique commune.

Disposition binaire, ABI/protocole/instructions, plages entières et coûts propres à un
algorithme de preuve restent dans leurs modèles sémantiques. Initialisations d’état, indices,
indications d’allocation et entrées de tests indépendantes ne sont pas automatiquement des
politiques. Ne remplacez pas une borne attendue indépendante par la constante testée.

Vérifiez déclarations et usages littéraux, gardes multilignes, boucles, macros, défauts,
minuteries et liaisons. Contrôlez sens et responsabilité de chaque politique touchée ; `Max*`
seul ne suffit pas. Un regroupement conserve types, valeurs, sens des comparaisons, sentinelles,
épuisement/erreurs. Séparez les changements de politique et lancez les [tests concernés](testing.md).

<!-- i18n-section: artifacts -->
### Documentation et artefacts locaux

Markdown doit être concis et vérifiable dans les sources, avec liens relatifs internes.
Actualisez les docs dans la même PR pour les changements CLI/API, support, options ou tests.
Synchronisez toutes les langues de ce guide ; relisez chaque traduction avant d’actualiser
le SHA-256 `i18n-source`. Conservez sections, exemples, API/options et destinations.
Exécutez `python3 scripts/check_docs_i18n.py` pour vérifier liens et parité.

Gardez rapports locaux de performance, traces brutes et temps dans les répertoires ignorés
de build ou benchmark-results. Commitez outils de mesure réutilisables et tests de régression,
pas les rapports locaux ou tableaux copiés.

<!-- i18n-section: tests -->
## Exécuter les tests

Exécutez tous les tests enregistrés via la cible agrégée :

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

Pendant le développement, utilisez la plus petite cible pertinente ou un label
CTest :

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

Le [guide des tests](testing.md) décrit toutes les cibles pratiques, les
suites de transformation accessibles uniquement par label, les expressions
régulières de test unique, la compilation des fixtures et les allers-retours
Unicorn. Si une cible est ignorée faute de compilateur croisé ou de linker,
signalez cette limite ; ne présentez pas le chemin non exécuté comme réussi.

<!-- i18n-section: python-sdk -->
### SDK de plugins Python

Pour modifier le SDK, lancez d’abord les suites Python pur et de dérive, puis les tests d’adaptateurs natifs :

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Déclarations C publiques, signatures ABI Python, versions paquet/CMake et politique de livraison
forment un contrat revu ensemble ; modifiez-les simultanément. Voir le [guide des plugins Python](python-plugins.md).

<!-- i18n-section: review -->
## Liste de contrôle d’une pull request

Avant de demander une revue :

- Rebasez ou fusionnez le dernier `dev` selon le flux préféré des mainteneurs
  et résolvez délibérément les changements de sous-modules.
- Compilez les cibles concernées en Release, ou expliquez pourquoi un autre
  profil est nécessaire.
- Exécutez les tests de régression précis et la suite pertinente la plus large
  possible ; indiquez les commandes exactes et tous les skips dans la
  description de la PR.
- Préservez le lifting strict : une instruction non prise en charge ne doit pas
  devenir silencieusement une opération devinée ni un `NOP`.
- Ajoutez une couverture sémantique aux changements de comportement, pas
  seulement des instantanés textuels d’IR.
- Écartez du diff le nettoyage sans rapport, les fichiers générés et les
  artefacts de compilation locaux.
- Vérifier les [règles d’en-têtes et de responsabilité](#style-du-code), gardes/défauts numériques
  et dérive des liaisons ; réussir le formatage ne suffit pas.
- Mettez à jour la documentation publique et contributeur lorsque le
  comportement, le support, les drapeaux, les commandes ou la responsabilité
  des tests changent.

Pour les signalements sensibles qui ne doivent pas commencer par une pull
request publique, suivez [SECURITY.md](../../SECURITY.md).
