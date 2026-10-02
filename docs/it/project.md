**Lingue**: [English](../../README.md) | [简体中文](../zh-CN/project.md) | [繁體中文](../zh-TW/project.md) | [日本語](../ja/project.md) | [한국어](../ko/project.md) | [Français](../fr/project.md) | [Deutsch](../de/project.md) | [Español](../es/project.md) | [Italiano](project.md) | [Русский](../ru/project.md) | [العربية](../ar/project.md)

<!-- i18n-source: 826aa66a4b07aee78f67638fb98159a69d7886c21769e56b6a0d6196a1437b63 -->

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

Formati di input, contratti host e limiti sono documentati nelle guide [EVM](evm.md) e [Solana SBF](sbf.md).

La CLI sperimentale `neverd mobile app.apk -o recovered-app` recupera Java da APK (multidex), DEX e smali e genera `report.json`. Il recupero Android usa esclusivamente il motore integrato di NeverD in C++20 e non richiede runtime Python o Java. Racchiudere tra virgolette i percorsi con spazi. La [guida Android](android.md) descrive input supportati, report e limiti di recupero.

Il flusso iOS sperimentale `neverd mobile App.ipa -o recovered-ios` esporta C nativo e sorgenti Objective-C/Swift supportati da IPA, `.app` o Mach-O. Layout runtime, unità sorgente e omissioni per metodo rimangono espliciti; il codice generato non usa ponti verso il binario originale. Configurazione, copertura e ricompilazione indipendente sono nella [guida iOS](ios.md).

Il [recupero sperimentale del sorgente degli interpreti](interpreter-recovery.md) usa `neverd decompile --devirtualize --func ENTRY` per specializzare gli interpreti x64 ELF/PE collegati supportati in HighC o LLVMC tramite la pipeline LowIR/MedIR comune. Gli indizi di controllo separano i contesti del decoder senza fissare gli input di esecuzione. Controllo irrisolto, semantica non supportata e budget esauriti causano errori espliciti; questa modalità non certifica la sostituzione del binario né l’equivalenza delle eccezioni.

I budget di recupero sono espliciti: `--vm-max-fields`, `--vm-max-refinements` e `--vm-max-queries` mantengono i valori predefiniti 16, 16 e 4096. La guida descrive l’API C v3 compatibile e le regole di errore.

Il recupero espone anche `--vm-chain-transfers=N` (predefinito 0) e `--vm-no-control-discovery`. Il concatenamento mantiene le correlazioni simboliche tra trasferimenti con destinazione unica dimostrata; al limite torna ai normali confini CFG. Il recupero dello stato macchina può dichiarare offset rispetto a RSP d’ingresso senza riavvolgimento, non verificati a runtime, con `--vm-entry-frame=begin:end`. La premessa numerica esatta accompagna C e rapporto; non autorizza memoria né dimostra equivalenza.

Il recupero dello stato macchina accetta `--vm-entry-alignment=A:R` come dominio esplicito e verificato del RSP iniziale. `A` deve essere una potenza positiva di due e `R < A`. Gli altri valori restituiscono stato 2 prima di accessi guest o scritture dello stato. I bit alti restano liberi e non si presume alcun allineamento predefinito. Questa opzione non certifica l’equivalenza nativa.

`--vm-external-stores-disjoint-frame` aggiunge una precondizione esplicita non verificata a runtime: l’intera estensione di ogni STORE esterno deve evitare `--vm-entry-frame`. Conserva solo fatti già presenti nell’intervallo. Non limita LOAD o gli alias fra puntatori esterni; il comportamento predefinito resta conservativo. Le API di prova nativa rifiutano questo dominio.

Le grandi funzioni recuperate che superano il limite di costruzione SSA possono usare `--llvm` tramite un contratto limitato di memoria scalare mutabile. Input iniziali, valori trasportati dai cicli e letture precedenti mantengono il proprio significato. Stati impliciti non supportati, parametri in registri vettoriali, rilocazioni, memoria ambigua e controllo malformato falliscono esplicitamente; HighC rifiuta questo percorso alternativo. L’output segue ancora il contratto esistente dello stato macchina e non aggiunge certificati di equivalenza.

L’API C++ separata per le prove dei cicli inferisce invarianti limitati e ranghi lessicografici per cicli annidati, poi ricontrolla il raffinamento dal nativo a LowIR. Consultare la [guida al recupero](interpreter-recovery.md); non certifica il C emesso.

L’API C++ separata `checkBinaryLLVMRefinement` compone nuove verifiche native e LLVM su un artefatto LLVM esatto; la compilazione C resta fuori dalla prova.

Il recupero PE autentica anche i byte DIR64 alla base preferita ed esclude le scritture delle importazioni; il contratto non certifica ASLR né inizializzazione.

Nel contratto esplicito dello stato macchina, il recupero supporta partizioni limitate dell’allineamento dello stack d’ingresso e la pulizia interna `RET imm16`. La composizione automatica delle prove native-to-LLVM per queste partizioni resta da completare.

Il recupero limitato di `REP MOVS/STOS` conserva ordine degli elementi e sovrapposizioni; la prova delle istruzioni originali resta da completare.

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

L’esecuzione CPU separa ammissione ISA, memoria guest, trasporto del backend e politiche OS. `NEVERD_ENABLE_CPU_EMULATION` attiva il livello CPU x64/ARM64; `NEVERD_ENABLE_DRIVER_EMULATION` aggiunge l’ambiente Windows WDM/KMDF x64 limitato. `linux-elf64-v1` esegue processi Linux ELF supportati. Vedere [Esecuzione CPU](cpu-execution.md), [Emulazione dei processi guest](process-emulation.md) e [Emulazione dei driver Windows](driver-emulation.md).

`windows-pe64-v1` aggiunge processi console Windows x64/ARM64 limitati: caricamento PE, PEB/TEB, TLS statico e dinamico, callback di avvio/uscita e modelli Win32 nominativi. Usa il livello CPU indipendentemente dai driver; caricamento DLL/CRT, GUI, SEH utente, thread e compatibilità Windows generale restano incompleti.

La memoria virtuale Windows aggiunge `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualQuery` e `FlushInstructionCache` per il processo corrente. Il livello OS gestisce le prenotazioni; `AddressSpace` resta responsabile delle pagine impegnate, dei permessi e della memoria sottostante. I test verificano modifiche al codice, errori di accesso e riutilizzo del budget di memoria.

`driver-strict` / `checked-x64-v1` supporta KVM su host Linux x64 compatibili e WHP su host Windows x64 compatibili; `auto` sceglie quel trasporto nativo, mentre ISA diverse usano Unicorn. Unicorn esplicito e la precedente API V1 mantengono il profilo software portabile. L’esecuzione nativa verifica indirizzi canonici ed effetti prima dell’ingresso; hardware assente produce un errore senza ripiego. Istruzioni e comportamento OS non supportati falliscono esplicitamente. La CI nativa Windows x64 con Unicorn disattivato supera tutti i 359 controlli obbligatori: 131 controlli CPU, 224 risultati di driver da 26 immagini integrate, 46 immagini WDK e 40 casi di scenario alle basi preferite e rilocate, più quattro controlli dei limiti SEH ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Mancano prove native ARM64; non è stabilita la compatibilità universale dei driver o Android/Darwin.

Il profilo x64 verificato include `MOVS/STOS/LODS` sulla RAM ordinaria e `CLD/STD`, con ripresa, annullamento e verifica delle pagine per elemento. I bit alti a conteggio nullo specifici della CPU e gli operandi di dispositivo STOS/LODS restano fuori dal contratto.

Il profilo x64 verificato supporta anche `CMPS/SCAS` sulla RAM ordinaria con `REPE/REPNE`, flag aritmetici, uscita anticipata, arresti per elemento e ripresa dopo errore. I confronti su dispositivi restano esclusi.

`checked-aarch64-v1` e `checked-user-aarch64-v1` offrono ARM64 FP32/FP64 e SIMD fissi limitati, con stato FPCR/FPSR/vettoriale completo. Linux ARM64 corrispondente usa KVM, Windows ARM64 usa WHP e ISA diverse usano Unicorn. Le prove native ARM64 restano pendenti; il caricamento di driver Windows resta x64.

Le sonde native x64 e ARM64 verificano esecuzione completa limitata con diritto esclusivo sulla memoria. Pacchetti XSAVE e cache di tabelle identificate per ISA hanno un’autorità unica; le prove dei carichi nativi ARM64 restano incomplete.

I campi x64 nativi `FOP/FIP/FDP` seguono le regole di salvataggio/ripristino dell’host: AMD può azzerare metadati x87 inattivi. Le sonde di avvio li verificano con un’eccezione pendente non mascherata.

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
  -DNEVERD_LLVM_PREBUILT_TAG=neverd-llvm-v23.0.0-r3
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

La revisione predefinita è `neverd-llvm-v23.0.0-r3`. Tag Git, destinazione del rilascio, commit sorgente e digest dei tre archivi formano un riferimento sorgente versionato immutabile. Le directory che conservano il vecchio tag base, `neverd-llvm-v23.0.0-r1` o `neverd-llvm-v23.0.0-r2` passano automaticamente a `r3`, salvo impostazione esplicita di `NEVERD_LLVM_PREBUILT_SHA256`. `Prebuilt LLVM Audit` viene eseguito su push, pull request e ogni sei ore; richiama `scripts/audit_prebuilt_llvm_release.py` per confrontare il riferimento con il rilascio GitHub attuale e ogni file di checksum pubblicato.

Se il fork LLVM cambia mentre LLVM riporta ancora `23.0.0`, pubblicare la revisione successiva del pacchetto, `neverd-llvm-v23.0.0-r4` e poi `-r5`, senza sovrascrivere un rilascio esistente o inventare la versione LLVM `23.0.1`:

```bash
gh workflow run neverd-release.yml \
  --repo NeverSight/llvm-project \
  --ref main \
  -f release_tag=neverd-llvm-v23.0.0-r4 \
  -f overwrite_existing_assets=false
```

Dopo il successo del workflow, aggiornare insieme tag predefinito, commit fissato e tre digest in `cmake/NeverDLLVMPrebuilt.cmake`. Il nuovo pacchetto viene conservato in `.cache/neverd-llvm/<tag>`; gli archivi obsoleti o ripubblicati falliscono prima dell’estrazione. `overwrite_existing_assets` serve solo al recupero storico e rimane disabilitato nel flusso normale.

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

L’[ambiente Qt Quick (inglese)](../gui.md) opzionale offre viste agganciabili di istruzioni, CFG, esadecimale, C e IR, tutte le 11 lingue dell’interfaccia, annotazioni salvate e connessioni MCP. L’analisi viene eseguita in un processo separato senza Qt; le build solo CLI rimangono indipendenti. Il [registro di qualificazione (inglese)](../gui-qualification.md) descrive i flussi supportati e le verifiche di piattaforma ancora necessarie prima del rilascio.

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
neverd patch --from-c repl.c --func 0x401000 -o patched binary
neverd patch --mode inplace -o patched binary
neverd patch --subst --flatten --mba -o patched binary
```

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
