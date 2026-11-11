**Langues**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](../it/project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: edadf84089252d2d8a80ae25926c8013d54fb4a3fea7fc631f3731016652375c -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Le moteur d’analyse et de décompilation AI-friendly — lift 1:1, basé sur LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK C + Python

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#construction)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-et-plugins)

[Documentation](README.md) · [Android](android.md) · [iOS](ios.md) · [Feuille de route](roadmap.md) · [Contribution](CONTRIBUTING.md)

</div>

---

> GitHub affiche toujours le `README.md` anglais sur la page du dépôt. Utilisez les liens de langue ci-dessus pour les versions localisées.

<!-- i18n-section: overview -->

## Vue d’ensemble

NeverD est un moteur d’analyse et de décompilation natif et smart-contract centré sur le **lifting d’instructions 1:1**. Il charge **PE**, **ELF**, **Mach-O**, le bytecode legacy **EVM** et les programmes Solana **SBF ELF**. Les cibles natives utilisent [Capstone](https://www.capstone-engine.org/) ; EVM et SBF ont leurs decoders versionnés et IR par étapes. Tous les parcours emploient des sémantiques manuscrites. Les instructions conservent leur comportement en **LLVM IR**, **C**, **Rust pour SBF**, **reconstruction Solidity pour EVM**, ou dans un **binaire natif réécrit**.

Le mode strict est **activé par défaut**. Une instruction sans lifter lève `UnliftedInstruction` au lieu de sauter, deviner, ou émettre un `NOP` silencieux.

CLI, intégrateurs et agents IA utilisent un seul moteur — **`libneverd`** — via une **API C pure**. Ils ne lient pas Capstone, LLVM, ni le C++ interne directement.

Consultez les guides [EVM](evm.md), [Solana SBF](sbf.md), [récupération de sources mobiles](mobile.md) et [récupération expérimentale de sources d’interpréteurs](interpreter-recovery.md) pour les usages, contrats pris en charge et limites.

<!-- i18n-section: why-neverd -->

## Pourquoi NeverD ?

- **Sémantique 1:1** — lifters manuscrits ; opcodes non supportés lèvent une exception en mode strict par défaut
- **Compatible LLM** — C structuré, LLVM IR et analyse JSON via une API C pure, avec des erreurs déterministes
- **Un pipeline, plusieurs sorties** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → binaire natif réécrit
- **Réécriture binaire** — PE / ELF / Mach-O, trampolines de section ou écrasement inplace
- **Boîte à outils d’analyse** — CLI, infos de debug, signatures, plugins, et passes d’obfuscation optionnelles

<!-- i18n-section: supported-targets -->

## Cibles prises en charge

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Chaque cellule de la matrice est implémentée, mais la profondeur des tests d’intégration varie. Consultez la [matrice de couverture de l’architecture](architecture.md#support-and-test-depth). Mach-O i386 utilise des objets relogeables `thin`, car macOS moderne ne peut pas lier les anciens exécutables i386.

**L’analyse ELF MIPS est expérimentale :** MIPS32r2/o32 et MIPS64r2/n64, dans les deux ordres d’octets, prennent en charge un sous-ensemble strict d’instructions entières pour le désassemblage, la reconstruction du CFG et les sorties C et LLVM. La réécriture de code natif n’est pas prise en charge. Voir [la couverture MIPS et l’exemple de firmware (anglais)](../mips.md) pour les sous-ensembles microMIPS32, MIPS16e/e2, DSP et MIPS32r5 MSA validés séparément et leurs restrictions de sortie source.

Le bytecode EVM legacy est pris en charge indépendamment des conteneurs natifs :
les 150 opcodes attribués de Frontier à Fusaka alimentent Low/Med/High IR,
LLVM `i256` vérifié, C23 `_BitInt(256)` et Solidity. Voir
[décompilation EVM](evm.md).

Les programmes Solana SBF v0-v4 ELF utilisent un loader strict dédié, des
métadonnées ISA versionnées complètes, Low/Med/High IR, LLVM vérifié, C11
portable et Rust stable sûr. Voir la [décompilation Solana SBF](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Récupération de sources mobiles

La CLI expérimentale `neverd mobile` prend en charge les entrées et sorties suivantes :

| Plateforme | Entrées | Sorties |
|------------|---------|---------|
| [Android](android.md) | APK, y compris multidex, DEX, fichiers ou répertoires smali | Sources Java et rapport JSON |
| [iOS](ios.md) | IPA, `.app`, Mach-O (arm64/x86_64) | C natif, sources Objective-C/Swift prises en charge et rapport de couverture JSON |

La récupération dépend des motifs de code pris en charge ; consultez la [vue d’ensemble mobile (anglais)](../mobile.md) et les guides de plateforme pour la couverture et les limites.

<!-- i18n-section: cpu-workloads -->

### Exécution CPU et environnements invités

NeverD sépare les backends CPU des modèles d’OS invités pour les processus bornés, les bibliothèques natives Android et les pilotes Windows x64. Unicorn exécute x64/ARM64 en logiciel ; les hôtes de même ISA peuvent utiliser KVM (Linux), WHP (Windows) ou HVF (macOS). Voir [Exécution CPU et charges invitées](emulation.md) pour les environnements, validations et limites.

<!-- i18n-section: how-it-works -->

## Fonctionnement

```text
Binary (PE / ELF / Mach-O)
  → Loader + DebugInfo
  → Capstone decode
  → LowIR     architecture-neutral NdOps · CFG
  → MedIR     types · ABI · calls · memory · SSA
       │
       ├─ lift        MedIR → LLVM IR
       ├─ decompile   MedIR → HighIR → C
       │              MedIR → LLVM IR → opt → C   (-llvm)
       └─ patch       MedIR → LLVM IR → codegen → binary

EVM (raw / hex / compiler artifact)
  → normalisation runtime + décodage sensible au hardfork
  → EVM LowIR → EVM stack-SSA MedIR → EVM HighIR récupéré
       ├─ lift        → LLVM i256/i512 vérifié
       └─ decompile   → C23 _BitInt(256) ou reconstruction Solidity

Solana SBF ELF (v0-v4)
  → loader legacy/strict sensible à la version + verifier
  → SBF LowIR → MedIR normalisé → SBF HighIR récupéré
       ├─ lift        → ABI runtime LLVM i64 vérifiée
       └─ decompile   → C11 portable ou Rust stable sûr
```

| Étape | Rôle |
|-------|------|
| **LowIR** | ~77 opcodes `NdOp` + CFG |
| **MedIR** | Types, conventions d’appel, modèle mémoire, SSA |
| **HighIR** | Contrôle structuré (`if` / `while` / `for`) |
| **LLVM** | Optimiser, émettre du C, ou générer du code machine |

<!-- i18n-section: quick-start -->

## Démarrage rapide

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Pseudo-code dans le langage du programme (C++, Rust, Go ou C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# Transformations facultatives d’une fonction native (HighC ordinaire les ignore)
./build/bin/neverd decompile --func main --symbolic-simplify -o enhanced.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=O3 -o optimized.c binary
./build/bin/neverd decompile --func main --llvm --llvm-opt=custom \
  --llvm-passes=instcombine,early-cse,simplifycfg,dce -o cleaned.c binary

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Android : APK vers Java (expérimental)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Analyse
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Les bibliothèques de signatures sont installées dans `build/bin/signatures/` à la compilation. `sigs --auto` choisit l’ensemble selon format, architecture et bitness. Pour un fichier PE dont l’en-tête Rich indique la version de Visual Studio de son éditeur de liens, il ne charge que le `vs<year>.pat` de cette version, en plus des fichiers qui n’appartiennent à aucune version. `--sig-base <dir>` choisit de la même façon dans une autre arborescence de signatures. Un fichier de motifs de 1 Mio ou plus n'est analysé qu'une fois : ses modules sont conservés dans `neverd/signatures`, sous le répertoire de cache de l'utilisateur, et projetés en mémoire lors des chargements suivants. `NEVERD_SIGNATURE_CACHE` désigne un autre répertoire, et `off` désactive le cache.

<!-- i18n-section: building -->

## Construction

**Prérequis :** CMake ≥ 3.20 · Ninja · compilateur C++20 · submodules Git (fork LLVM + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

La première configuration compile le fork LLVM localement (souvent 30–60 min). Ensuite, builds incrémentaux. Presets : `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>LLVM précompilé · artefacts · tests · options CMake</strong></summary>

<br>

**LLVM précompilé**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

La CI habituelle de NeverD, sur push et pull request, compile délibérément le sous-module LLVM depuis les sources. Lors d'un lancement manuel du workflow `CI`, cochez `use_prebuilt_llvm` pour valider les paquets publiés ; seul un `true` choisi manuellement active le LLVM précompilé. Sans cette case, le chemin reste la compilation depuis les sources, comme en CI automatique.

Le paquet publié est choisi selon l'hôte qui exécute CMake :

| Hôte | Artefact de release |
|------|---------------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Chaque archive est vérifiée avec le condensat fixé dans `cmake/NeverDLLVMPrebuilt.cmake`, ou son fichier `.sha256` publié pour un tag non décrit par ces valeurs, avant extraction sous `~/.cache/neverd-llvm/<tag>/<arch>/` ou `NEVERD_LLVM_PREBUILT_CACHE_DIR`. Pour la version fixée par défaut, `BUILDINFO.txt` doit aussi nommer le commit exact du sous-module LLVM. Les builds de publication utilisent ccache sur macOS/Linux et sccache avec le cache GitHub Actions pour clang-cl sous Windows. Ces caches accélèrent seulement la reconstruction et ne sont jamais publiés comme artefacts.

La révision par défaut est `neverd-llvm-v23.0.0-r6` : tag Git, cible de publication, commit source et trois condensats d’archives forment une référence source versionnée immuable. Les builds qui conservent l’ancien tag de base, `neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`, `neverd-llvm-v23.0.0-r3`, `neverd-llvm-v23.0.0-r4` ou `neverd-llvm-v23.0.0-r5` migrent automatiquement vers `r6`, sauf surcharge explicite `NEVERD_LLVM_PREBUILT_SHA256`. Le workflow `Prebuilt LLVM Audit` s’exécute lors des push, des pull requests et toutes les six heures. Il appelle `scripts/audit_prebuilt_llvm_release.py` pour comparer cette référence à la publication GitHub actuelle et à chaque fichier de somme de contrôle.

Si le fork LLVM change tout en annonçant `23.0.0`, publiez la révision de paquet suivante, `neverd-llvm-v23.0.0-r7`, puis `-r8`, sans écraser une publication existante ni inventer la version LLVM `23.0.1` :

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

Après réussite, mettez à jour ensemble le tag par défaut, le commit fixé et les trois condensats dans `cmake/NeverDLLVMPrebuilt.cmake`. Chaque révision du paquet utilise son propre répertoire de cache ; la révision par défaut actuelle se trouve sous `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. Une archive périmée ou republiée échoue avant extraction. `overwrite_existing_assets` sert uniquement à la récupération historique et reste désactivé dans le workflow normal.

**Artefacts**

| Chemin | Description |
|--------|-------------|
| `build/bin/neverd` | CLI unifiée |
| `build/bin/neverd-bench` | Banc de mesure (JSON) |
| `build/bin/neverd-sigmaker` | Générateur `.pat` depuis bibliothèques statiques |
| `build/bin/libneverd.*` | Bibliothèque partagée du moteur |
| `build/bin/sdk/` | Racine d’inclusion canonique du SDK C ; utilisez `<neverd/sdk/NeverDCAPI.h>` ou `<neverd/sdk/NeverDPlugin.h>` en conservant la hiérarchie `neverd/sdk/` |
| `build/bin/sdk/python/` | Paquet de plugins Python typé et exemples |
| `build/bin/signatures/` | Bibliothèques de signatures |

**Tests**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Cible | Description |
|-------|-------------|
| `check-neverd` | Tous les tests |
| `check-neverd-semantic` | Roundtrip sémantique seul (Unicorn) |

Pour les cibles ciblées, les labels CTest, les exigences des fixtures et la grille de réécriture multiformat, consultez [Tester NeverD](testing.md).

**Options CMake**

| Option | Défaut | Description |
|--------|--------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM précompilé CI |
| `NEVERD_BUILD_SHARED` | `ON` | Construire `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | Intégrer les plugins CPython 3.10+ |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugins d’exemple |
| `BUILD_TESTING` | `OFF` | Tests unitaires |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Groupe de tests sémantiques dépendant de Unicorn (avec `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Atelier de bureau

L’[atelier de bureau (anglais)](../gui.md) optionnel reprend la disposition et les raccourcis du désassembleur interactif classique, avec des vues de désassemblage, de graphe, de pseudo-code, d’IR, hexadécimale et de listes, un thème inspiré de Visual Studio Code, les 11 langues d’interface et des bases de projet `.nddb`. L’analyse s’exécute dans un processus distinct sans Qt ; les builds CLI restent indépendants. Consultez le [dossier de qualification (anglais)](../gui-qualification.md) pour les workflows pris en charge et les validations de plateforme encore nécessaires avant publication.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Pipeline

| Commande | Sortie | Description |
|----------|--------|-------------|
| `lift` | `.ll` | Lever vers LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, Solidity EVM ou Rust SBF choisi avec `--language` |
| `decompile -llvm` | `.c` | Via LLVM IR + optimiseur |
| `decompile --devirtualize` | `.c` + JSON facultatif | Récupération expérimentale d’interpréteurs x64 ; `--func` requis ; [contrat et exemples](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Expérimental: [Android](android.md), [iOS](ios.md) |
| `patch` | binaire | Réécrire le code machine |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Les correctifs source peuvent utiliser Clang, le mode ordinaire de NeverC ou GCC, selon les [profils source/object vérifiés](../source-patching.md). Les objets natifs conservent le code machine du producteur ; la compatibilité du compilateur et du runtime ainsi que la propagation des exceptions sont vérifiées pour chaque profil.

Pour un binaire ARM 32 bits sans métadonnées ARM/Thumb d’une fonction, déclarez le mode d’entrée avant la décompilation :

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Répétez l’option pour les autres entrées ambiguës, avec `:arm` si nécessaire. Une déclaration incompatible avec les métadonnées binaires vérifiées fait échouer le chargement ; une déclaration valide ne concerne que l’entrée exacte. L’API C expose le même réglage préalable via `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>Commandes d’analyse</strong></summary>

<br>

| Commande | Rôle |
|----------|------|
| `info` / `dashboard` / `headers` | Métadonnées et aperçu |
| `funcs` | Fonctions découvertes |
| `disasm` | Désassemblage (`--func` nom ou hex) |
| `sym-explore` | Exploration bornée des chemins LowIR natifs (`--func` ; sortie JSON) |
| `audit` | Défauts de durée de vie du tas et lectures non initialisées de la pile locale (JSON) |
| `hunt` | Débordements de copies dangereuses avec témoins symboliques et preuves de rejeu `process-input-v1` supplémentaires lorsqu’un plan complet est disponible (schéma JSON v1) |
| `hex` | Dump hexadécimal à une adresse |
| `cfg` / `callgraph` | CFG / graphe d’appels (JSON ; DOT/SVG optionnel) |
| `xrefs` | Références croisées |
| `switches` | Tables de sauts des switch issues de l'analyse du programme entier |
| `strings` / `search` | Chaînes / recherche octets ou texte |
| `imports` / `exports` / `symbols` / `relocs` | Tables |
| `segments` / `sections` / `entrypoints` | Disposition |
| `diff` | Comparer deux binaires (`-a` / `-b`) |
| `sigs` | Signatures (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Annotations de session |
| `export` | Exporter les résultats |
| `plugins` | Lister ou exécuter des plugins |

La plupart des commandes d’analyse acceptent `--json`.

</details>

<!-- i18n-section: sdk-and-plugins -->

## SDK et plugins

Les intégrateurs utilisent l’**API C pure** de `libneverd` :

| En-tête | Rôle |
|---------|------|
| `NeverDCAPI.h` | Session, lift, décompilation, patch, IR / CFG, annotations |
| `NeverDPlugin.h` | ABI plugin en bibliothèque dynamique |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Pour EVM, `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` sélectionne
explicitement Solidity ; `neverd_decompile_all` continue à produire du C. Voir
les [exemples API C EVM](evm.md#c-api).

Les bibliothèques partagées natives et les fichiers Python `.py` utilisent le
même cycle de vie des plugins. Compilez l’exemple natif avec
`-DNEVERD_BUILD_PLUGINS=ON` ; consultez le
[guide des plugins natifs](plugins.md) pour le descripteur en C pur, les
callbacks, les étapes de compilation/liaison, la découverte, le parcours CLI
et les contraintes d’ABI. Python est activé par défaut et peut être entièrement
retiré avec `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF` ; le
[guide des plugins Python](python-plugins.md) couvre son SDK typé et son
workflow de paquet. Les deux types utilisent `<neverd-dir>/plugins`,
`~/.neverd/plugins` et `$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Dépendances

| Composant | Rôle | Source |
|-----------|------|--------|
| **LLVM** (fork) | IR, optimisation, codegen, diagnostics | `third_party/llvm-project` ou précompilé |
| **Capstone** | Décodage | `third_party/capstone` |

Les composants tiers conservent leurs propres licences.

<!-- i18n-section: contributing -->

## Contribution

Les contributions sont intégrées dans la branche **`dev`**. Consultez le [guide de contribution](CONTRIBUTING.md) pour la configuration, les procédures Release/Debug, le style, les tests ciblés et les exigences des pull requests. Les guides d’[architecture](architecture.md) et de [test](testing.md) relient les changements courants au code et aux suites de validation correspondants.

<!-- i18n-section: license -->

## Licence

[GNU AGPL version 3 uniquement](../../LICENSE). La redistribution de code NeverD couvert ou d’adaptations doit conserver les avis de droits d’auteur, de licence et d’absence de garantie, y compris l’attribution et la source du projet dans [NOTICE](../../NOTICE). Cela concerne aussi la réutilisation assistée par IA/LLM et les transformations fondées sur LLVM.

Voir [Attribution et citation](ATTRIBUTION.md) pour les exigences, le périmètre et les exemples. Pour assurer la traçabilité, nous recommandons de citer le fichier source et la version ou le commit exact. [CITATION.cff](../../CITATION.cff) fournit les métadonnées de citation du logiciel ; citer ne remplace pas le respect de la licence.

Les composants LLVM conservent leur licence Apache-2.0 WITH LLVM-exception. Capstone conserve sa propre licence.
