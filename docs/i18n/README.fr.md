**Langues**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Le moteur d’analyse et de décompilation AI-friendly — lift 1:1, basé sur LLVM**

PE · ELF · Mach-O · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · SBF &nbsp;|&nbsp; SDK C pur

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#construction)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O%20%7C%20SBF-informational.svg)](#cibles-prises-en-charge)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM%20%7C%20SBF-orange.svg)](#cibles-prises-en-charge)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-et-plugins)

[Documentation](../README.fr.md) · [Feuille de route](../roadmap/README.fr.md) · [Contribution](CONTRIBUTING.fr.md)

</div>

---

> GitHub affiche toujours le `README.md` anglais sur la page du dépôt. Utilisez les liens de langue ci-dessus pour les versions localisées.

## Vue d’ensemble

NeverD est un moteur d’analyse et de décompilation de binaires natifs et de smart contracts centré sur le **lifting d’instructions 1:1**. Il charge **PE**, **ELF**, **Mach-O** et les programmes Solana **SBF ELF**. Les cibles natives sont décodées avec [Capstone](https://www.capstone-engine.org/) ; SBF utilise un decoder dédié sensible à la version et un IR par étapes. Tous les parcours emploient des sémantiques écrites à la main plutôt qu’une traduction approximative. Les instructions prises en charge conservent leur comportement en **LLVM IR**, **C structuré**, **Rust stable sûr pour SBF**, ou dans un **binaire natif réécrit**.

Le mode strict est **activé par défaut**. Une instruction sans lifter lève `UnliftedInstruction` au lieu de sauter, deviner, ou émettre un `NOP` silencieux.

CLI, intégrateurs et agents IA utilisent un seul moteur — **`libneverd`** — via une **API C pure**. Ils ne lient pas Capstone, LLVM, ni le C++ interne directement.

La décompilation Solana SBF est disponible ; consultez le [guide SBF](../sbf.fr.md). Les autres cibles et travaux de durcissement sont suivis dans la [feuille de route](../roadmap/README.fr.md).

## Pourquoi NeverD ?

- **Sémantique 1:1** — lifters manuscrits ; opcodes non supportés lèvent une exception en mode strict par défaut
- **Compatible LLM** — C structuré, LLVM IR et analyse JSON via une API C pure, avec des erreurs déterministes
- **Un pipeline, plusieurs sorties** — `lift` → LLVM IR · `decompile` → C/Rust · `patch` → binaire natif réécrit
- **Réécriture binaire** — PE / ELF / Mach-O, trampolines de section ou écrasement inplace
- **Boîte à outils d’analyse** — CLI, infos de debug, signatures, plugins, et passes d’obfuscation optionnelles

## Cibles prises en charge

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Chaque cellule de la matrice est implémentée, mais la profondeur des tests d’intégration varie. Consultez la [matrice de couverture de l’architecture](../architecture.fr.md#support-and-test-depth). Mach-O i386 utilise des objets relogeables `thin`, car macOS moderne ne peut pas lier les anciens exécutables i386.

Les programmes Solana SBF v0-v4 ELF utilisent un loader strict dédié, des
métadonnées ISA versionnées complètes, Low/Med/High IR, LLVM vérifié, C11
portable et Rust stable sûr. Voir la [décompilation Solana SBF](../sbf.fr.md).

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

## Démarrage rapide

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Analyse
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

Les bibliothèques de signatures sont installées dans `build/bin/signatures/` à la compilation. `sigs --auto` choisit l’ensemble selon format, architecture et bitness.

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**Artefacts**

| Chemin | Description |
|--------|-------------|
| `build/bin/neverd` | CLI unifiée |
| `build/bin/neverd-bench` | Banc de mesure (JSON) |
| `build/bin/neverd-sigmaker` | Générateur `.pat` depuis bibliothèques statiques |
| `build/bin/libneverd.*` | Bibliothèque partagée du moteur |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
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

Pour les cibles ciblées, les labels CTest, les exigences des fixtures et la grille de réécriture multiformat, consultez [Tester NeverD](../testing.fr.md).

**Options CMake**

| Option | Défaut | Description |
|--------|--------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM précompilé CI |
| `NEVERD_BUILD_SHARED` | `ON` | Construire `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugins d’exemple |
| `BUILD_TESTING` | `OFF` | Tests unitaires |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Pipeline

| Commande | Sortie | Description |
|----------|--------|-------------|
| `lift` | `.ll` | Lever vers LLVM IR |
| `decompile` | `.c` / `.rs` | C ou Rust SBF choisi avec `--language` |
| `decompile -llvm` | `.c` | Via LLVM IR + optimiseur |
| `patch` | binaire | Réécrire le code machine |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Commandes d’analyse</strong></summary>

<br>

| Commande | Rôle |
|----------|------|
| `info` / `dashboard` / `headers` | Métadonnées et aperçu |
| `funcs` | Fonctions découvertes |
| `disasm` | Désassemblage (`--func` nom ou hex) |
| `hex` | Dump hexadécimal à une adresse |
| `cfg` / `callgraph` | CFG / graphe d’appels (JSON ; DOT/SVG optionnel) |
| `xrefs` | Références croisées |
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

Construire le plugin d’exemple avec `-DNEVERD_BUILD_PLUGINS=ON`. Chemins de chargement : `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dépendances

| Composant | Rôle | Source |
|-----------|------|--------|
| **LLVM** (fork) | IR, optimisation, codegen, diagnostics | `third_party/llvm-project` ou précompilé |
| **Capstone** | Décodage | `third_party/capstone` |

Les composants tiers conservent leurs propres licences.

## Contribution

Les contributions sont intégrées dans la branche **`dev`**. Consultez le [guide de contribution](CONTRIBUTING.fr.md) pour la configuration, les procédures Release/Debug, le style, les tests ciblés et les exigences des pull requests. Les guides d’[architecture](../architecture.fr.md) et de [test](../testing.fr.md) relient les changements courants au code et aux suites de validation correspondants.

## Licence

[AGPL-3.0](../../LICENSE)

Les composants LLVM conservent leur licence Apache-2.0 WITH LLVM-exception. Capstone conserve sa propre licence.
