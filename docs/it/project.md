**Lingue**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: eba5aa249bd6a7b45bc98a51faf985724d6cdfbb4dc21091fce89d6f541d01b8 -->

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/neverd-logo-dark.svg">
  <img src="../assets/neverd-logo-light.svg" width="72" alt="NeverD">
</picture>

# NeverD

**Il motore di analisi e decompilazione AI-friendly — lift 1:1, basato su LLVM**

PE · ELF · Mach-O · EVM · Solana SBF &nbsp;|&nbsp; x86-64 · i386 · AArch64 · ARM32 · EVM256 · SBF &nbsp;|&nbsp; SDK C + Python

[![AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](../../LICENSE)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-brightgreen.svg)](#compilazione)
![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux%20%7C%20Windows-informational.svg)
[![SDK](https://img.shields.io/badge/SDK-C%20%2B%20Python-orange.svg)](#sdk-e-plugin)

[Documentazione](README.md) · [Android](android.md) · [iOS](ios.md) · [Roadmap](roadmap.md) · [Contribuire](CONTRIBUTING.md)

</div>

---

> GitHub mostra sempre il `README.md` inglese sulla homepage del repository. Usa i link lingua sopra per le versioni localizzate.

<!-- i18n-section: overview -->

## Panoramica

NeverD è un motore di analisi e decompilazione native e smart-contract basato sul **lifting istruzione per istruzione 1:1**. Carica **PE**, **ELF**, **Mach-O**, bytecode legacy **EVM** e programmi Solana **SBF ELF**. I target nativi usano [Capstone](https://www.capstone-engine.org/); EVM e SBF hanno decoder version-aware e IR a stadi dedicati. Ogni percorso usa semantiche scritte a mano. Le istruzioni conservano il comportamento in **LLVM IR**, **C**, **Rust per SBF**, **ricostruzione Solidity per EVM** o in un **binario nativo riscritto**.

La modalità strict è **attiva di default**. Un’istruzione senza lifter lancia `UnliftedInstruction` invece di saltare, indovinare o emettere un `NOP` silenzioso.

CLI, integratori e agent AI usano un solo motore — **`libneverd`** — tramite una **API C pura**. Non collegano Capstone, LLVM o il C++ interno direttamente.

Le guide [EVM](evm.md), [Solana SBF](sbf.md), [recupero di sorgenti mobili](mobile.md) e [recupero sperimentale di interpreti](interpreter-recovery.md) descrivono uso, contratti supportati e limiti.

<!-- i18n-section: why-neverd -->

## Perché NeverD?

- **Semantica 1:1** — lifter scritti a mano; gli opcode non supportati lanciano eccezione in strict di default
- **Compatibile con LLM** — C strutturato, LLVM IR e analisi JSON tramite API C pura, con errori deterministici
- **Una pipeline, più uscite** — `lift` → LLVM IR · `decompile` → C/Solidity/Rust · `patch` → binario nativo riscritto
- **Riscrittura binaria** — PE / ELF / Mach-O con trampolini di sezione o overwrite inplace
- **Toolkit di analisi** — CLI, debug info, firme, plugin e pass di obfuscation opzionali

<!-- i18n-section: supported-targets -->

## Target supportati

| | **x86-64** | **i386** | **AArch64** | **ARM32** |
|---|:---:|:---:|:---:|:---:|
| **PE** (Windows) | ✓ | ✓ | ✓ | ✓ |
| **ELF** (Linux / Android) | ✓ | ✓ | ✓ | ✓ |
| **Mach-O** (macOS / iOS) | ✓ | ✓ | ✓ | ✓ |

> Ogni cella della matrice è implementata, ma la profondità dei test d’integrazione varia. Consulta la [matrice di copertura dell’architettura](architecture.md#support-and-test-depth). Mach-O i386 usa oggetti `thin` rilocabili perché macOS moderno non può collegare gli eseguibili i386 storici.

**L’analisi ELF MIPS è sperimentale:** MIPS32r2/o32 e MIPS64r2/n64, in entrambi gli ordini dei byte, supportano un sottoinsieme rigoroso di istruzioni intere per disassemblaggio, ricostruzione del CFG e output C e LLVM. La riscrittura del codice nativo non è supportata. Vedi [la copertura MIPS e l’esempio firmware (inglese)](../mips.md).

Il bytecode EVM legacy è supportato indipendentemente dai container nativi: i
150 opcode assegnati da Frontier a Fusaka alimentano Low/Med/High IR, LLVM
`i256` verificato, C23 `_BitInt(256)` e Solidity. Vedi
[decompilazione EVM](evm.md).

I programmi Solana SBF v0-v4 ELF usano un loader strict dedicato, metadata ISA
versionati completi, Low/Med/High IR, LLVM verificato, C11 portabile e Rust
stabile e sicuro. Vedi [decompilazione Solana SBF](sbf.md).

<!-- i18n-section: mobile-source-recovery -->

### Recupero del sorgente mobile

La CLI sperimentale `neverd mobile` supporta i seguenti input e output:

| Piattaforma | Input | Output |
|-------------|-------|--------|
| [Android](android.md) | APK, incluso multidex, DEX, file o directory smali | Sorgenti Java e report JSON |
| [iOS](ios.md) | IPA, `.app`, Mach-O (arm64/x86_64) | C nativo, sorgenti Objective-C/Swift supportati e report di copertura JSON |

Il recupero dipende dai modelli di codice supportati; per copertura e limiti consultare la [panoramica mobile (inglese)](../mobile.md) e le guide di piattaforma.

<!-- i18n-section: cpu-workloads -->

### Esecuzione CPU e ambienti guest

NeverD separa i backend CPU dai modelli del sistema ospite per processi limitati, librerie native Android e driver Windows x64. Unicorn esegue x64/ARM64 via software; gli host con la stessa ISA possono usare KVM (Linux), WHP (Windows) o HVF (macOS). Vedi [Esecuzione CPU e carichi ospiti](emulation.md) per ambienti, verifiche e limiti.

<!-- i18n-section: how-it-works -->

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

<!-- i18n-section: quick-start -->

## Avvio rapido

```bash
git submodule update --init --recursive
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Pipeline
./build/bin/neverd lift -o out.ll binary
./build/bin/neverd decompile -o out.c binary
./build/bin/neverd patch -hello -o patched binary

# Pseudocodice nel linguaggio del programma (C++, Rust, Go o C)
./build/bin/neverd decompile --language=source -o out.rs rust-binary
./build/bin/neverd decompile --language=cpp -o out.cpp cpp-binary
./build/bin/neverd decompile --language=go --func main.main go-binary

# Trasformazioni facoltative di una funzione nativa (HighC ordinario le omette)
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

# Android: da APK a Java (sperimentale)
./build/bin/neverd mobile app.apk -o recovered-android
./build/bin/neverd mobile App.ipa -o recovered-ios

# Analisi
./build/bin/neverd funcs binary
./build/bin/neverd disasm --func 0x401000 binary
./build/bin/neverd sym-explore --func 0x401000 --expressions binary
./build/bin/neverd audit binary
./build/bin/neverd hunt binary
./build/bin/neverd sigs --auto binary
```

Le librerie di firme vengono installate in `build/bin/signatures/` a build time. `sigs --auto` sceglie il set da formato, architettura e bitness. Se l’intestazione Rich di un file PE indica la versione di Visual Studio del suo linker, carica solo il `vs<year>.pat` di quella versione, oltre ai file che non appartengono a nessuna versione. `--sig-base <dir>` sceglie allo stesso modo da un altro albero di firme. Un file di pattern da 1 MiB in su viene analizzato una sola volta: i suoi moduli sono conservati in `neverd/signatures`, nella directory di cache dell'utente, e mappati nei caricamenti successivi. `NEVERD_SIGNATURE_CACHE` indica un'altra directory, e `off` disattiva la cache.

<!-- i18n-section: building -->

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r6
cmake --build build
```

La CI ordinaria di NeverD, su push e pull request, compila deliberatamente il sottomodulo LLVM dai sorgenti. Avviando manualmente il workflow `CI`, selezionare `use_prebuilt_llvm` per validare i pacchetti pubblicati; solo un `true` scelto a mano abilita l'LLVM prebuilt. Lasciandolo deselezionato resta lo stesso percorso di build dai sorgenti della CI automatica.

Il pacchetto pubblicato viene scelto in base all'host che esegue CMake:

| Host | Artefatto di release |
|------|----------------------|
| macOS arm64 | `neverd-llvm-macos-arm64.tar.xz` |
| Linux x86_64 | `neverd-llvm-linux-x86_64.tar.xz` |
| Windows x64 | `neverd-llvm-windows-x64.zip` |

Ogni archivio viene verificato rispetto al digest fissato in `cmake/NeverDLLVMPrebuilt.cmake`, oppure al `.sha256` pubblicato per tag non descritti da tali valori, prima dell’estrazione in `~/.cache/neverd-llvm/<tag>/<arch>/` o `NEVERD_LLVM_PREBUILT_CACHE_DIR`. Per la versione predefinita, anche `BUILDINFO.txt` deve indicare il commit esatto del sottomodulo LLVM. Le build di rilascio usano ccache su macOS/Linux e sccache con la cache di GitHub Actions per clang-cl su Windows. Le cache accelerano solo la ricompilazione e non sono mai pubblicate come artefatti.

La revisione predefinita è `neverd-llvm-v23.0.0-r6`. Tag Git, destinazione del rilascio, commit sorgente e digest dei tre archivi formano un riferimento sorgente versionato immutabile. Le directory che conservano il vecchio tag base, `neverd-llvm-v23.0.0-r1`, `neverd-llvm-v23.0.0-r2`, `neverd-llvm-v23.0.0-r3`, `neverd-llvm-v23.0.0-r4` o `neverd-llvm-v23.0.0-r5` passano automaticamente a `r6`, salvo impostazione esplicita di `NEVERD_LLVM_PREBUILT_SHA256`. `Prebuilt LLVM Audit` viene eseguito su push, pull request e ogni sei ore; richiama `scripts/audit_prebuilt_llvm_release.py` per confrontare il riferimento con il rilascio GitHub attuale e ogni file di checksum pubblicato.

Se il fork LLVM cambia mentre LLVM riporta ancora `23.0.0`, pubblicare la revisione successiva del pacchetto, `neverd-llvm-v23.0.0-r7` e poi `-r8`, senza sovrascrivere un rilascio esistente o inventare la versione LLVM `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r7 \
  -f overwrite_existing_assets=false
```

Dopo il successo del workflow, aggiornare insieme tag predefinito, commit fissato e tre digest in `cmake/NeverDLLVMPrebuilt.cmake`. Ogni revisione del pacchetto usa una propria directory di cache; quella predefinita attuale si trova in `.cache/neverd-llvm/neverd-llvm-v23.0.0-r6`. Gli archivi obsoleti o ripubblicati falliscono prima dell’estrazione. `overwrite_existing_assets` serve solo al recupero storico e rimane disabilitato nel flusso normale.

**Artefatti**

| Percorso | Descrizione |
|----------|-------------|
| `build/bin/neverd` | CLI unificata |
| `build/bin/neverd-bench` | Benchmark (JSON) |
| `build/bin/neverd-sigmaker` | Generatore `.pat` da librerie statiche |
| `build/bin/libneverd.*` | Libreria condivisa del motore |
| `build/bin/sdk/` | Root include canonica del C SDK; usare `<neverd/sdk/NeverDCAPI.h>` o `<neverd/sdk/NeverDPlugin.h>` mantenendo la gerarchia `neverd/sdk/` |
| `build/bin/sdk/python/` | Pacchetto tipizzato di plugin Python ed esempi |
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

Per target mirati, etichette CTest, requisiti delle fixture e griglia di riscrittura tra formati, consulta [Testare NeverD](testing.md).

**Opzioni CMake**

| Opzione | Default | Descrizione |
|---------|---------|-------------|
| `NEVERD_LLVM_PREBUILT` | `OFF` | LLVM prebuilt CI |
| `NEVERD_BUILD_SHARED` | `ON` | Compila `libneverd` |
| `NEVERD_ENABLE_PYTHON_PLUGINS` | `ON` | Incorporare il supporto ai plugin CPython 3.10+ |
| `NEVERD_BUILD_PLUGINS` | `OFF` | Plugin di esempio |
| `BUILD_TESTING` | `OFF` | Unit test |
| `NEVERD_ENABLE_SEMANTIC_TESTS` | `ON` | Gruppo di test semantici dipendente da Unicorn (con `BUILD_TESTING=ON`) |

</details>

<!-- i18n-section: desktop-workbench -->

## Ambiente desktop

L’[ambiente desktop (inglese)](../gui.md) opzionale segue la disposizione e le scorciatoie del disassemblatore interattivo classico, con viste di disassemblato, grafo, pseudocodice, IR, esadecimale ed elenchi, un tema in stile Visual Studio Code, tutte le 11 lingue dell’interfaccia e database di progetto `.nddb`. L’analisi viene eseguita in un processo separato senza Qt; le build solo CLI rimangono indipendenti. Il [registro di qualificazione (inglese)](../gui-qualification.md) descrive i flussi supportati e le verifiche di piattaforma ancora necessarie prima del rilascio.

<!-- i18n-section: cli -->

## CLI

```text
neverd <command> [options] <binary>
```

<!-- i18n-section: pipeline -->

### Pipeline

| Comando | Output | Descrizione |
|---------|--------|-------------|
| `lift` | `.ll` | Lift a LLVM IR |
| `decompile` | `.c` / `.sol` / `.rs` | C, Solidity EVM o Rust SBF selezionato con `--language` |
| `decompile -llvm` | `.c` | Via LLVM IR + ottimizzatore |
| `decompile --devirtualize` | `.c` + JSON opzionale | Recupero sperimentale di interpreti x64; richiede `--func`; [contratto ed esempi](interpreter-recovery.md) |
| `mobile` | `.java` / `.c` / `.m` / `.swift` + JSON | Sperimentale: [Android](android.md), [iOS](ios.md) |
| `patch` | binario | Riscrittura del codice macchina |

```bash
neverd decompile program --func vm_entry --devirtualize --vm-control=r10 \
  --recovery-report recovery.json -o recovered.c
neverd patch -hello -o patched binary
neverd patch --from-ir repl.ll -o patched binary
neverd patch --from-source repl.c --entry replacement --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

Le patch del sorgente possono usare Clang, la modalità ordinaria di NeverC o GCC secondo i [profili source/object verificati](../source-patching.md). Gli oggetti nativi conservano il codice macchina del produttore; la compatibilità tra compilatore e runtime e la propagazione delle eccezioni vengono verificate per ogni profilo.

Se un binario ARM a 32 bit non contiene i metadati ARM/Thumb di una funzione, dichiarare la modalità di ingresso prima della decompilazione:

```bash
neverd decompile --arm-function-mode=0xADDRESS:thumb -o output.c binary
```

Ripetere l’opzione per altri ingressi ambigui, usando `:arm` quando opportuno. Le dichiarazioni in conflitto con metadati binari verificati impediscono il caricamento; quelle valide riguardano solo l’ingresso esatto. L’API C espone la stessa impostazione preliminare con `neverd_session_set_arm_function_mode()`.

<details>
<summary><strong>Comandi di analisi</strong></summary>

<br>

| Comando | Scopo |
|---------|-------|
| `info` / `dashboard` / `headers` | Metadati e panoramica |
| `funcs` | Funzioni scoperte |
| `disasm` | Disassembla (`--func` nome o hex) |
| `sym-explore` | Esplorazione limitata dei percorsi LowIR nativi (`--func`; output JSON) |
| `audit` | Difetti di durata degli oggetti heap e letture non inizializzate dello stack locale (JSON) |
| `hunt` | Overflow di copie pericolose con testimoni simbolici e prove aggiuntive di riproduzione `process-input-v1` quando è disponibile un piano completo (schema JSON v1) |
| `hex` | Hex dump a un indirizzo |
| `cfg` / `callgraph` | CFG / call graph (JSON; DOT/SVG opzionale) |
| `xrefs` | Cross-reference |
| `switches` | Tabelle di salto degli switch dall'analisi dell'intero programma |
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

<!-- i18n-section: sdk-and-plugins -->

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
[esempi C API EVM](evm.md#c-api).

Le librerie condivise native e i file Python `.py` usano lo stesso ciclo di vita
dei plugin. Compila l’esempio nativo con `-DNEVERD_BUILD_PLUGINS=ON`; consulta
la [guida ai plugin nativi](plugins.md) per il descrittore C puro, le
callback, i passaggi di build/link, il rilevamento, il flusso CLI e i vincoli
dell’ABI. Il supporto Python è attivo per impostazione predefinita e può essere
rimosso completamente con `-DNEVERD_ENABLE_PYTHON_PLUGINS=OFF`; la
[guida ai plugin Python](python-plugins.md) tratta l’SDK tipizzato e il
flusso del pacchetto. Entrambi i tipi usano `<neverd-dir>/plugins`,
`~/.neverd/plugins` e `$NEVERD_PLUGIN_PATH`.

<!-- i18n-section: dependencies -->

## Dipendenze

| Componente | Ruolo | Sorgente |
|------------|-------|----------|
| **LLVM** (fork) | IR, ottimizzazione, codegen, diagnostica | `third_party/llvm-project` o prebuilt |
| **Capstone** | Decode | `third_party/capstone` |

I componenti di terze parti mantengono le proprie licenze.

<!-- i18n-section: contributing -->

## Contribuire

I contributi vengono integrati nel branch **`dev`**. Consulta la [guida per contribuire](CONTRIBUTING.md) per configurazione, istruzioni Release/Debug, stile, test mirati e requisiti delle pull request. Le guide di [architettura](architecture.md) e [test](testing.md) collegano le modifiche comuni al codice e alle suite di validazione corrispondenti.

<!-- i18n-section: license -->

## Licenza

[GNU AGPL solo versione 3](../../LICENSE). La ridistribuzione del codice NeverD coperto o di adattamenti deve conservare gli avvisi di copyright, licenza ed esclusione di garanzia, inclusi attribuzione e origine del progetto in [NOTICE](../../NOTICE). Vale anche per il riuso assistito da IA/LLM e per le trasformazioni basate su LLVM.

Requisiti, ambito ed esempi sono in [Attribuzione e citazione](ATTRIBUTION.md). Per la tracciabilità consigliamo di citare il file sorgente e la versione o il commit esatto. [CITATION.cff](../../CITATION.cff) contiene metadati di citazione del software; la citazione non sostituisce il rispetto della licenza.

I componenti LLVM mantengono la licenza Apache-2.0 WITH LLVM-exception. Capstone mantiene la propria licenza.
