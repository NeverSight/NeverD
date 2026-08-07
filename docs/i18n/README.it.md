**Lingue**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Il motore di analisi e decompilazione AI-friendly — lift 1:1, basato su LLVM**

PE · ELF · Mach-O &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 &nbsp;|&nbsp; SDK C puro

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilazione)
[![Formats](https://img.shields.io/badge/Formats-PE%20%7C%20ELF%20%7C%20Mach--O-informational.svg)](#target-supportati)
[![Arch](https://img.shields.io/badge/Arch-x86__64%20%7C%20i386%20%7C%20AArch64%20%7C%20ARM-orange.svg)](#target-supportati)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-lightgrey.svg)](#sdk-e-plugin)

[Documentazione](../README.it.md) · [Roadmap](../roadmap/README.it.md) · [Contribuire](#contribuire)

</div>

---

> GitHub mostra sempre il `README.md` inglese sulla homepage del repository. Usa i link lingua sopra per le versioni localizzate.

## Panoramica

NeverD è un motore di analisi e decompilazione di binari nativi basato su **lifting istruzione per istruzione 1:1**. Carica **PE**, **ELF** e **Mach-O**, decodifica con [Capstone](https://www.capstone-engine.org/) e solleva attraverso una pipeline IR a quattro stadi con **semantiche scritte a mano** — non una traduzione approssimativa. L’obiettivo è la **fedeltà semantica al 100%**: le istruzioni supportate mantengono il comportamento osservabile completo in **LLVM IR**, **C strutturato** o un **binario riscritto**.

La modalità strict è **attiva di default**. Un’istruzione senza lifter lancia `UnliftedInstruction` invece di saltare, indovinare o emettere un `NOP` silenzioso.

CLI, integratori e agent AI usano un solo motore — **`libneverd`** — tramite una **API C pura**. Non collegano Capstone, LLVM o il C++ interno direttamente.

Le prossime versioni aggiungeranno la decompilazione [EVM](../roadmap/README.it.md#2-decompilazione-bytecode-evm) e [Solana eBPF / SBF](../roadmap/README.it.md#3-decompilazione-solana-ebpf-sbf) sullo stesso stack IR — vedi la [roadmap](../roadmap/README.it.md).

## Perché NeverD?

- **Semantica 1:1** — lifter scritti a mano; gli opcode non supportati lanciano eccezione in strict di default
- **Compatibile con LLM** — C strutturato, LLVM IR e analisi JSON tramite API C pura, con errori deterministici
- **Una pipeline, tre uscite** — `lift` → LLVM IR · `decompile` → C · `patch` → binario riscritto
- **Riscrittura binaria** — PE / ELF / Mach-O con trampolini di sezione o overwrite inplace
- **Toolkit di analisi** — CLI, debug info, firme, plugin e pass di obfuscation opzionali

## Target supportati

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> La copertura d’integrazione Mach-O i386 usa oggetti `thin` rilocabili e test del backend di riscrittura degli eseguibili; l’host macOS corrente non può collegare gli eseguibili i386 storici.

## Come funziona

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
```

| Stadio | Ruolo |
|--------|-------|
| **LowIR** | ~77 opcode `NdOp` + CFG |
| **MedIR** | Tipi, calling convention, modello di memoria, SSA |
| **HighIR** | Control flow strutturato (`if` / `while` / `for`) |
| **LLVM** | Ottimizza, emette C, o genera codice macchina |

## Avvio rapido

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Analisi
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sigs --auto binary
```

Le librerie di firme vengono installate in `build/bin/signatures/` a build time. `sigs --auto` sceglie il set da formato, architettura e bitness.

## Compilazione

**Requisiti:** CMake ≥ 3.20 · Ninja · compilatore C++20 · Git submodule (LLVM fork + Capstone)

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

La prima configurazione compila il fork LLVM in locale (spesso 30–60 minuti). Le build successive sono incrementali. Preset: `CMakePresets.json` → `release` / `relwithdebinfo` / `debug`.

<details>
<summary><strong>LLVM prebuilt · artefatti · test · opzioni CMake</strong></summary>

<br>

**LLVM prebuilt**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEVERD_LLVM_PREBUILT=ON \
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0
cmake --build build
```

**Artefatti**

| Percorso | Descrizione |
|----------|-------------|
| `build/bin/neverd` | CLI unificata |
| `build/bin/neverd-bench` | Benchmark (JSON) |
| `build/bin/neverd-sigmaker` | Generatore `.pat` da librerie statiche |
| `build/bin/libneverd.*` | Libreria condivisa del motore |
| `build/bin/sdk/` | `NeverDCAPI.h`, `NeverDPlugin.h` |
| `build/bin/signatures/` | Librerie di firme incluse |

**Test**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target check-neverd
```

| Target | Descrizione |
|--------|-------------|
| `check-neverd` | Tutti i test |
| `check-neverd-semantic` | Solo roundtrip semantico (Unicorn) |

**Opzioni CMake**

| Opzione | Default | Descrizione |
|---------|---------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM prebuilt CI |
| `NEVERD_BUILD_SHARED` | `ON` | Compila `libneverd` |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugin di esempio |
| `BUILD_TESTING` | `OFF` | Unit test |

</details>

## CLI

```text
neverd <command> [options] <binary>
```

### Pipeline

| Comando | Output | Descrizione |
|---------|--------|-------------|
| `lift` | `.ll` | Lift a LLVM IR |
| `decompile` | `.c` | C strutturato (HighIR) |
| `decompile -llvm` | `.c` | Via LLVM IR + ottimizzatore |
| `patch` | binario | Riscrittura del codice macchina |

```bash
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

<details>
<summary><strong>Comandi di analisi</strong></summary>

<br>

| Comando | Scopo |
|---------|-------|
| `info` / `dashboard` / `headers` | Metadati e panoramica |
| `funcs` | Funzioni scoperte |
| `disasm` | Disassembla (`--func` nome o hex) |
| `hex` | Hex dump a un indirizzo |
| `cfg` / `callgraph` | CFG / call graph (JSON; DOT/SVG opzionale) |
| `xrefs` | Cross-reference |
| `strings` / `search` | Stringhe / ricerca byte o testo |
| `imports` / `exports` / `symbols` / `relocs` | Tabelle |
| `segments` / `sections` / `entrypoints` | Layout |
| `diff` | Confronta due binari (`-a` / `-b`) |
| `sigs` | Firme (`--auto`) |
| `rename` / `annotate` / `bookmarks` | Annotazioni di sessione |
| `export` | Esporta risultati |
| `plugins` | Elenca o esegue plugin |

La maggior parte dei comandi di analisi accetta `--json`.

</details>

## SDK e plugin

Gli integratori usano l’**API C pura** di `libneverd`:

| Header | Ruolo |
|--------|-------|
| `NeverDCAPI.h` | Sessione, lift, decompile, patch, IR / CFG, annotazioni |
| `NeverDPlugin.h` | ABI plugin a libreria dinamica |

```c
neverd_session_t s = neverd_session_create();
neverd_session_load(s, "binary.exe");
neverd_session_analyze(s);

const char *c = neverd_decompile(s, 0x401000);
neverd_free_string(c);
neverd_session_destroy(s);
```

Compila il plugin di esempio con `-DNEVERD_BUILD_PLUGINS=ON`. Percorsi di load: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dipendenze

| Componente | Ruolo | Sorgente |
|------------|-------|----------|
| **LLVM** (fork) | IR, ottimizzazione, codegen, diagnostica | `third_party/llvm-project` o prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

I componenti di terze parti mantengono le proprie licenze.

## Contribuire

Lo stile segue le convenzioni LLVM (`.clang-format`).

Lo sviluppo avviene sul branch **`dev`** (branch predefinito su GitHub).

```bash
git clone -b dev https://github.com/NeverSight/NeverD.git
cd NeverD
git submodule update --init --recursive
```

## Licenza

[AGPL-3.0](../../LICENSE)

I componenti LLVM mantengono la licenza Apache-2.0 WITH LLVM-exception. Capstone mantiene la propria licenza.
