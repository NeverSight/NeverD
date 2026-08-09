**Langues**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# Contribuer à NeverD

NeverD est un projet d’analyse binaire qui donne la priorité à la sémantique.
Une contribution utile reste ciblée, fait échouer explicitement les
comportements non pris en charge et inclut le plus petit test qui prouve le
contrat modifié.

Avant toute modification, lisez le [guide d’architecture](../architecture.fr.md).
Consultez le [guide des tests](../testing.fr.md) pour choisir une suite et la
[feuille de route](../roadmap/README.fr.md) pour les travaux produit planifiés.

## Prérequis

- Git avec prise en charge des sous-modules récursifs
- CMake 3.20 ou version ultérieure
- Ninja
- Un compilateur C++20
- Clang et LLD (`ld.lld` et `lld-link`) pour l’ensemble complet de fixtures
  multi-cibles

Les sous-modules récursifs fournissent les forks LLVM et Capstone de NeverD,
Unicorn et les données de signatures. Ne les remplacez pas par des révisions
système arbitraires lors de la validation d’une modification.

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

Ne publiez jamais un débit de décodage ou de lifting mesuré avec une
compilation Debug. Utilisez Release pour les benchmarks, ou RelWithDebInfo si
le profilage nécessite des symboles.

### LLVM précompilé sur macOS

Les contributeurs sur Apple Silicon peuvent éviter de compiler localement le
fork LLVM :

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake télécharge le paquet de version configuré par le dépôt, vérifie sa somme
SHA-256 et réutilise ensuite le cache utilisateur extrait. Le canal précompilé
prend uniquement en charge macOS arm64. Les Mac Intel et les builds universels
doivent employer la compilation LLVM locale par défaut. Les options avancées,
comme `NEVERD_LLVM_PREBUILT_TAG`, l’URL du miroir, le répertoire de cache et une
somme explicite, sont documentées dans `cmake/NeverDLLVMPrebuilt.cmake`.

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

## Style du code

Le C et le C++ suivent les conventions LLVM, avec `.clang-format` comme
autorité de formatage du dépôt. Formatez uniquement les fichiers modifiés :

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

Ne reformatez pas tout le dépôt pour une correction ciblée. Suivez les
conventions de nommage et de découpage voisines, gardez les comportements
spécifiques à une plateforme à la frontière loader/lifter/backend appropriée
et n’exposez pas de types C++ internes via le SDK C pur.

Le Markdown doit être concis et vérifiable depuis le code source. Utilisez des
liens relatifs pour les fichiers du dépôt et mettez la documentation à jour
dans la même pull request lorsqu’un comportement CLI, une API publique, une
affirmation de support, un drapeau de compilation ou une commande de test
change.

## Exécuter les tests

Exécutez tous les tests enregistrés via la cible agrégée :

```bash
cmake --build build-release --target check-neverd
```

Pendant le développement, utilisez la plus petite cible pertinente ou un label
CTest :

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

Le [guide des tests](../testing.fr.md) décrit toutes les cibles pratiques, les
suites de transformation accessibles uniquement par label, les expressions
régulières de test unique, la compilation des fixtures et les allers-retours
Unicorn. Si une cible est ignorée faute de compilateur croisé ou de linker,
signalez cette limite ; ne présentez pas le chemin non exécuté comme réussi.

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
- Mettez à jour la documentation publique et contributeur lorsque le
  comportement, le support, les drapeaux, les commandes ou la responsabilité
  des tests changent.

Pour les signalements sensibles qui ne doivent pas commencer par une pull
request publique, suivez [SECURITY.md](../../SECURITY.md).
