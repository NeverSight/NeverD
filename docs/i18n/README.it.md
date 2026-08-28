**Lingue**: [English](../../README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Il motore di analisi e decompilazione AI-friendly — lift 1:1, basato su LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK C puro

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilazione)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-Pure%20C%20API-orange.svg)](#sdk-e-plugin)

[Documentazione](../README.it.md) · [Roadmap](../roadmap/README.it.md) · [Contribuire](CONTRIBUTING.it.md)

</div>

---

> GitHub mostra sempre il `README.md` inglese sulla homepage del repository. Usa i link lingua sopra per le versioni localizzate.

## Panoramica

NeverD è un motore di analisi e decompilazione native e smart-contract basato sul **lifting istruzione per istruzione 1:1**. Carica **PE**, **ELF**, **Mach-O**, bytecode legacy **EVM** e programmi Solana **SBF ELF**. I target nativi usano [Capstone](https://www.capstone-engine.org/); EVM e SBF hanno decoder version-aware e IR a stadi dedicati. Ogni percorso usa semantiche scritte a mano. Le istruzioni conservano il comportamento in **LLVM IR**, **C**, **Rust per SBF**, **ricostruzione Solidity per EVM** o in un **binario nativo riscritto**.

La modalità strict è **attiva di default**. Un’istruzione senza lifter lancia `UnliftedInstruction` invece di saltare, indovinare o emettere un `NOP` silenzioso.

CLI, integratori e agent AI usano un solo motore — **`libneverd`** — tramite una **API C pura**. Non collegano Capstone, LLVM o il C++ interno direttamente.

Formati di input, contratti host e limiti sono documentati nelle guide [EVM](../evm.it.md) e [Solana SBF](../sbf.it.md).

## Perché NeverD?

- **Semantica 1:1** — lifter scritti a mano; gli opcode non supportati lanciano eccezione in strict di default
- **Compatibile con LLM** — C strutturato, LLVM IR e analisi JSON tramite API C pura, con errori deterministici
- **Una pipeline, più uscite** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → binario nativo riscritto
- **Riscrittura binaria** — PE / ELF / Mach-O con trampolini di sezione o overwrite inplace
- **Toolkit di analisi** — CLI, debug info, firme, plugin e pass di obfuscation opzionali

## Target supportati

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Ogni cella della matrice è implementata, ma la profondità dei test d’integrazione varia. Consulta la [matrice di copertura dell’architettura](../architecture.it.md#support-and-test-depth). Mach-O i386 usa oggetti `thin` rilocabili perché macOS moderno non può collegare gli eseguibili i386 storici.

Il bytecode EVM legacy è supportato indipendentemente dai container nativi: i
150 opcode assegnati da Frontier a Fusaka alimentano Low/Med/High IR, LLVM
`i256` verificato, C23 `_BitInt(256)` e Solidity. Vedi
[decompilazione EVM](../evm.it.md).

I programmi Solana SBF v0-v4 ELF usano un loader strict dedicato, metadata ISA
versionati completi, Low/Med/High IR, LLVM verificato, C11 portabile e Rust
stabile e sicuro. Vedi [decompilazione Solana SBF](../sbf.it.md).

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

EVM (raw / hex / compiler artifact)
  → normalizzazione runtime + decode hardfork-aware
  → EVM LowIR → EVM stack-SSA MedIR → EVM HighIR recuperato
       ├─ lift        → LLVM i256/i512 verificato
       └─ decompile   → C23 _BitInt(256) o ricostruzione Solidity

Solana SBF ELF (v0-v4)
  → loader legacy/strict consapevole della versione + verifier
  → SBF LowIR → MedIR normalizzato → SBF HighIR recuperato
       ├─ lift        → ABI runtime LLVM i64 verificata
       └─ decompile   → C11 portabile o Rust stabile e sicuro
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

# EVM
./build/bin/neverd lift contract.evm -o contract.ll
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Solana SBF
./build/bin/neverd info program.so
./build/bin/neverd lift program.so -o program.ll
./build/bin/neverd decompile --language=c program.so -o program.c
./build/bin/neverd decompile --language=rust program.so -o program.rs

# Analisi
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
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

La CI ordinaria di NeverD, su push e pull request, compila deliberatamente il sottomodulo LLVM dai sorgenti. Avviando manualmente il workflow `CI`, selezionare `use_prebuilt_llvm` per validare i pacchetti pubblicati; solo un `true` scelto a mano abilita l'LLVM prebuilt. Lasciandolo deselezionato resta lo stesso percorso di build dai sorgenti della CI automatica.

Il pacchetto pubblicato viene scelto in base all'host che esegue CMake:

| Host | Artefatto di release |
|------|----------------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Ogni archivio viene confrontato con il digest fissato in `cmake/NeverDLLVMPrebuilt.cmake` — oppure con il `.sha256` pubblicato accanto ad esso, per un tag che quei pin non descrivono — prima di essere estratto in `~/.cache/neverd-llvm/<tag>/<arch>/` (o nel percorso indicato da `NEVERD_LLVM_PREBUILT_CACHE_DIR`). La build di release usa ccache su macOS e Linux; le build clang-cl su Windows usano sccache con la cache di GitHub Actions come backend. Le cache del compilatore accelerano solo le ricompilazioni e non vengono mai pubblicate come artefatti.

Il tag di release versiona il pacchetto NeverD, mentre `BUILDINFO.txt` registra l'esatto commit del fork LLVM. Se LLVM continua a riportare `23.0.0` ma i sorgenti del fork sono cambiati, la scelta immutabile consueta è una revisione di pacchetto come `neverd-llvm-v23.0.0-r1` (poi `-r2`), non `23.0.1`, a meno che non sia cambiata la patch version di LLVM stesso. Puntare `NEVERD_LLVM_PREBUILT_TAG` a quella nuova revisione.

Per riparare sul posto la release mutabile `neverd-llvm-v23.0.0`, eseguire il workflow `NeverD LLVM Release` dal branch `main` di llvm-project abilitando `overwrite_existing_assets`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0 \
  -f overwrite_existing_assets=true
```

Questo sostituisce gli artefatti omonimi ma deliberatamente non sposta il tag Git esistente. Aggiornare nello stesso cambiamento i digest fissati in `cmake/NeverDLLVMPrebuilt.cmake`: sono quei digest, non il tag, a nominare la build che una revisione di NeverD si aspetta, così una `~/.cache/neverd-llvm/neverd-llvm-v23.0.0/` obsoleta viene sostituita alla configurazione successiva e un archivio che non corrisponde ad alcun digest fissato ferma quella configurazione con una discrepanza di checksum, invece di riaffiorare più tardi come un header che il pacchetto precedente non conteneva. Un nuovo tag `-rN` evita del tutto la riscrittura sul posto. Il workflow rifiuta una sostituzione accidentale finché la casella non è attiva e la rifiuta del tutto se GitHub marca la release come immutabile.

**Artefatti**

| Percorso | Descrizione |
|----------|-------------|
| `build/bin/neverd` | CLI unificata |
| `build/bin/neverd-bench` | Benchmark (JSON) |
| `build/bin/neverd-sigmaker` | Generatore `.pat` da librerie statiche |
| `build/bin/libneverd.*` | Libreria condivisa del motore |
| `build/bin/sdk/` | Root include canonica del C SDK; usare `<neverd/sdk/NeverDCAPI.h>` o `<neverd/sdk/NeverDPlugin.h>` mantenendo la gerarchia `neverd/sdk/` |
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

Per target mirati, etichette CTest, requisiti delle fixture e griglia di riscrittura tra formati, consulta [Testare NeverD](../testing.it.md).

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
| `decompile` | `.c` / `.sol` / `.rs` | C, Solidity EVM o Rust SBF selezionato con `--language` |
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
| `sym-explore` | Esplorazione limitata dei percorsi LowIR nativi (`--func`; output JSON) |
| `audit` | Difetti di vita dell’heap: leak, doppia free, use-after-free (JSON) |
| `hunt` | Overflow di copie pericolose con evidenza simbolica e valori candidati (JSON) |
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

Per EVM, `neverd_decompile_all_ex(..., NEVERD_OUTPUT_SOLIDITY, ...)` seleziona
Solidity esplicitamente; `neverd_decompile_all` continua a emettere C. Vedi gli
[esempi C API EVM](../evm.it.md#c-api).

Compila il plugin di esempio con `-DNEVERD_BUILD_PLUGINS=ON`. Percorsi di load: `<neverd-dir>/plugins`, `~/.neverd/plugins`, `$NEVERD_PLUGIN_PATH`.

## Dipendenze

| Componente | Ruolo | Sorgente |
|------------|-------|----------|
| **LLVM** (fork) | IR, ottimizzazione, codegen, diagnostica | `third_party/llvm-project` o prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

I componenti di terze parti mantengono le proprie licenze.

## Contribuire

I contributi vengono integrati nel branch **`dev`**. Consulta la [guida per contribuire](CONTRIBUTING.it.md) per configurazione, istruzioni Release/Debug, stile, test mirati e requisiti delle pull request. Le guide di [architettura](../architecture.it.md) e [test](../testing.it.md) collegano le modifiche comuni al codice e alle suite di validazione corrispondenti.

## Licenza

[AGPL-3.0](../../LICENSE)

I componenti LLVM mantengono la licenza Apache-2.0 WITH LLVM-exception. Capstone mantiene la propria licenza.
