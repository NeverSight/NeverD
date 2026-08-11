**Lingue**: [English](testing.md) | [简体中文](testing.zh-CN.md) | [繁體中文](testing.zh-TW.md) | [日本語](testing.ja.md) | [한국어](testing.ko.md) | [Français](testing.fr.md) | [Deutsch](testing.de.md) | [Español](testing.es.md) | [Italiano](testing.it.md) | [Русский](testing.ru.md) | [العربية](testing.ar.md)

[← Indice della documentazione](README.it.md)

# Testare NeverD

I test di NeverD rispondono a tre domande diverse: una rappresentazione ha la
forma prevista, un percorso completo funziona per una fixture binaria e il
codice generato conserva il comportamento? Scegli la suite minima che risponde
alla domanda della modifica, poi esegui l’aggregato più ampio prima di una pull
request ad alto rischio.

## Configurare una build di test

I test sono disabilitati se non si abilita `BUILD_TESTING`. Release è la scelta
normale per la suite completa; Debug mantiene assertion ed esecuzione
passo-passo, ma è intenzionalmente non ottimizzato e non rappresentativo per i
benchmark di decode.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

Il set completo di fixture richiede `clang` per la compilazione multi-target e
i linker LLVM (`ld.lld` e `lld-link`) nel `PATH`. CMake genera sempre molti
oggetti rilocabili e fixture ELF/PE collegate quando è disponibile il linker
corrispondente. Un test ignorato perché l’host non può compilare o collegare la
fixture è copertura non eseguita, non un superamento per quel target.

Consulta [CONTRIBUTING.md](i18n/CONTRIBUTING.it.md) per clonazione, profili di
build e LLVM precompilato su macOS.

## Organizzazione dei test

`add_neverd_unittest` crea un eseguibile GoogleTest e assegna a ogni caso
scoperto una label CTest uguale al nome del target eseguibile.

| Area sorgente | Target e label CTest | Copertura |
|---------------|----------------------|-----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | Invocazione di processi figlio multipiattaforma, quoting, redirect e codici di uscita |
| `unittests/libc` | `NeverDLibCTests` | Nomi libc noti e classificazione |
| `unittests/lift` | `NeverDLiftTests` | Forme LowIR decoder/lifter, fasi IR, loader, relocation, fixture di formato, decompilazione e flussi patch rappresentativi |
| La maggior parte di `unittests/semantic` | `NeverDSemanticTests` | Semantica differenziale di istruzioni, ABI, controllo, espressioni C e lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMAnalyzerTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalizzazione input, CFG/SSA/recovery, semantica interpreter, esecuzione differenziale LLVM/C/Solidity e API pubblica |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFSemanticTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFEmitterTests`, `NeverDSBFIntegrationTests` | Metadati v0-v4 e layout ELF, verifica rigorosa, CFG/recupero, esecuzione raw indipendente, verifica LLVM, compilazione C/Rust e instradamento dell’API pubblica |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalenza riscrittura/offuscamento su quattro ISA e tre formati oggetto |
| File di trasformazione mirati in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sonde veloci da ricollegare separate dal grande binario semantico |

Le fonti autorevoli per la registrazione sono
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) e
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) e
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt).

A ogni esecuzione, l’audit degli opcode EVM esegue un `git fetch` shallow del
`HEAD` remoto dal
[repository go-ethereum ufficiale](https://github.com/ethereum/go-ethereum),
quindi riporta il commit esatto verificato. Riutilizza la bare cache ignorata
`build/evm-opcode-audit/go-ethereum.git`, ma la aggiorna prima di leggere
l’inventario chiuso degli opcode e le assegnazioni dei byte:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

CI esegue lo stesso audit live a ogni push e pull request, su avvio manuale e
una volta al giorno, così rileva drift upstream anche quando NeverD non cambia.
Per una riproduzione offline o storica, seleziona esplicitamente un checkout
esistente:

```bash
python3 scripts/audit_evm_opcode_metadata.py \
  --geth-root /path/to/go-ethereum
```

L’audit consente soltanto le esclusioni nominate in
`EVMUpstreamOpcodePolicy.def`; un opcode upstream non rappresentato né
esplicitamente revisionato fa fallire il comando. Il parser e i diagnostic di
drift hanno una copertura unit Python indipendente in CI, eseguibile con:

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Per modifiche al control flow EVM, esegui prima il contratto di punto fisso e
dominio delle altezze:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Questi casi coprono return interni tra block, merge finiti con più target,
convergenza dei loop e ordine deterministico degli edge, stack height dipendenti
dal percorso, widening limitato, over-approximation cartesiana indotta dalla
correlazione, jump sconosciuti, target precisamente non validi e stack fault in
modalità strict e relaxed. Esegui poi tutti e sette i binari EVM e l’audit della
metadata upstream; modifiche al CFG possono influire su emitter e integration
anche quando la forma locale dell’analyzer è corretta.

Per modifiche al dataflow MedIR/HighIR, esegui anche i contratti constant-phi,
selector, typed-operand, malformed-graph e deep-chain:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

Questi casi provano phi ciclici uguali e in conflitto, espressioni selector non
adiacenti e tra block, entrambi gli ordini degli operandi di uguaglianza, check
esatti delle width ABI, operandi tipizzati storage/event/calldata, gestione
deterministica di MedIR malformed e un producer walk iterativo su 16.384 valori.

## Come vengono prodotte le fixture

### Fixture di lift e formato

`unittests/lift/CMakeLists.txt` compila sorgenti C e assembly per più target
durante la build. Le triple Clang producono oggetti ELF x86-64, i386, AArch64 e
ARM32, oggetti e immagini collegate PE/COFF e oggetti Mach-O i386 PIC/no-PIC.
Quando è disponibile LLD, alcuni oggetti vengono anche collegati in eseguibili
per i test patch. `NeverDLiftTests` dipende dal target `lift-test-objects`,
quindi una build normale di quel binario aggiorna le fixture generate.

La maggior parte dei test lift usa `NeverDLiftFixture.h` per invocare la CLI
`neverd` compilata e ispezionare LowIR, MedIR, HighIR, LLVM IR, C generato o un
binario riscritto. La variabile d’ambiente `NEVERD` può sovrascrivere il percorso
della CLI per un esperimento manuale mirato; le normali esecuzioni CTest usano
l’eseguibile incorporato da CMake.

### Ricostruzione delle eccezioni Windows

Le modifiche alle eccezioni Windows basate su tabelle richiedono sia test della
rappresentazione sia un test di patch su un PE collegato. Il filtro lift mirato
copre il modello unwind/SEH/C++ normalizzato, gli input corrotti, gli archi CFG
eccezionali, HighIR, la generazione LLVM WinEH, la sostituzione della directory
delle eccezioni e la ricostruzione Guard CF/EH continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture assembly x64 protetta richiede il target Windows di Clang e
`lld-link`; il link CMake usa `/guard:cf` e `/guard:ehcont`. Uno skip dovuto a
un cross-linker mancante non dimostra il percorso final-image. Un caso di
integrazione riuscito prova che il PE riscritto può essere ricaricato e che le
tabelle runtime-function, unwind, load-config, Guard CF e Guard EH continuation
restano ordinate, presenti nel file e limitate a target eseguibili.

La fixture FH3 collegata copre indipendentemente la chiusura C++ nativa: tabelle
di stato fisse, annotazioni HighC, conservazione della personality, target catch
generati e grafo IP-to-state ricaricato.

Vedere [Ricostruzione delle eccezioni Windows](windows-exception-reconstruction.it.md)
per la matrice di supporto analisi/nativo e il contratto di patch fail-closed.

### Roundtrip differenziali Unicorn

La fixture semantica verifica il comportamento anziché la forma testuale:

1. Scrivere un piccolo caso C/assembly o costruire LLVM IR.
2. Compilarlo con Clang/LLVM per il target richiesto.
3. Eseguire il codice macchina originale in Unicorn e acquisire il ritorno previsto o altro stato definito dalla fixture.
4. Caricarlo e fare lift con NeverD, emettere LLVM IR e ricompilare il risultato in codice macchina.
5. Eseguire il codice rigenerato con stessa ABI, input, layout di memoria e modello CPU.
6. Confrontare i risultati osservabili.

L’implementazione principale è
[`SemanticRoundTripFixture.h`](../unittests/semantic/SemanticRoundTripFixture.h).
La fixture patch-full usa `Codegen::compileForRewrite`, lo stesso backend di
riscrittura delle operazioni patch, poi confronta codice baseline e trasformato
sull’intera griglia ISA/formato 4×3.

Un errore semantico deterministico di NeverD deve far fallire il test. Riserva
gli skip a limiti espliciti di capacità esterna e leggine il motivo: un riepilogo
verde senza cross-linker non prova che il percorso del formato sia stato
eseguito.

### Backend differenziali EVM

I test interpreter forniscono un oracle deterministico a 256 bit. La suite
emitter compila ed esegue LLVM, abbassa C23 con Clang sullo stesso host harness
e, se sono disponibili `solc`, `anvil`, `cast` e `jq`, deploya Solidity generato
in locale. Confronta status, storage e trace count. Un corpus raw separato esegue
ALU pre-Fusaka, copie calldata/memory, `MCOPY` sovrapposto, Keccak e return data
nell’EVM nativa di Anvil.

`NeverDEVMOpcodeTests` impone anche l’architettura metadata: 150 opcode fanno
roundtrip tra encoding e valore tipizzato; vengono testati confini di famiglia,
alias hardfork e massimi stack/host derivati.

### Backend differenziali Solana SBF

I test dei metadati SBF convalidano ogni funzionalità di versione, i confini di collisione degli opcode, gli hash syscall Murmur3, le rilocazioni e le costanti di machine ELF, registro e indirizzo VM. Le fixture del loader generano, senza binari inclusi, sia layout legacy v0-v2 a sezioni sia layout rigorosi v3/v4 senza sezioni e basati sui program header.

`NeverDSBFSemanticTests` esegue direttamente byte di istruzioni verificati e non usa MedIR; modificare o corrompere l’IR normalizzato non può quindi far concordare accidentalmente il source oracle con un backend. Copre la semantica v2 non monotona, memoria, syscall, frame di chiamata interni, fault, trace e limiti di risorse. I moduli LLVM vengono verificati; il C generato è compilato con i warning come errori e Rust con `-D warnings`. I test dell’API pubblica attraversano tutti gli stadi IR, disassembly, CFG, metadati, LLVM, C e Rust partendo da un ELF SBF rigoroso generato.

## Target in un solo comando

I target personalizzati compilano le dipendenze e poi eseguono CTest con
parallelismo derivato dalle CPU host:

| Target CMake | Selezione |
|--------------|-----------|
| `check-neverd` | Tutti i test registrati |
| `check-neverd-semantic` | Solo `NeverDSemanticTests` |
| `check-neverd-sbf` | Tutti i target/casi `NeverDSBF*Tests` |
| `check-neverd-patch-full` | Solo `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | Solo `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | Solo `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | Solo `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` e `NeverDAvxUpperXformTests` al momento non hanno un
target di comodità `check-neverd-*`. Compilali e selezionali per label come
mostrato sotto. `check-neverd-semantic` inoltre non include i binari separati di
trasformazione o patch-full; usa `check-neverd` per l’aggregato completo.

## Flusso CTest incrementale

Compila prima l’eseguibile proprietario e poi selezionane la label. Evita così
di ricollegare grandi target semantici non correlati.

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# Tutti i target/casi EVM mirati
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMAnalyzerTests NeverDEVMSemanticTests NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Tutti i target/casi Solana SBF mirati
cmake --build build-release --target \
  NeverDSBFMetadataTests NeverDSBFLoaderTests NeverDSBFAnalyzerTests \
  NeverDSBFSemanticTests NeverDSBFLLVMEmitterTests NeverDSBFEmitterTests \
  NeverDSBFIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'SBF' --output-on-failure --parallel 4
```

Usa un nome CTest derivato da GoogleTest per una singola regressione:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

Selettori utili:

| Comando | Scopo |
|---------|-------|
| `ctest --test-dir build-release -N` | Elencare i casi scoperti senza eseguirli |
| `ctest --test-dir build-release -L '<regex>'` | Selezionare una label di binario test |
| `ctest --test-dir build-release -R '<regex>'` | Selezionare nomi di casi |
| `ctest --test-dir build-release --output-on-failure` | Mostrare diagnostica solo in caso di errore |
| `ctest --test-dir build-release --stop-on-failure` | Fermarsi dopo il primo errore |
| `ctest --test-dir build-release --parallel 4` | Eseguire fino a quattro casi in parallelo |

La discovery GoogleTest usa `DISCOVERY_MODE PRE_TEST`, quindi il binario
corrispondente deve esistere prima che CTest lo enumeri. I timeout per caso e di
discovery separati sono definiti in `cmake/AddNeverD.cmake` e vanno ampliati
solo per suite con casi pesanti misurati.

## Quali test cambiano con il codice?

| Area di modifica | Iniziare da | Poi considerare |
|------------------|-------------|-----------------|
| Lifter di architettura o decode | Caso nominato in `NeverDLiftTests` | Roundtrip semantico dell’ISA corrispondente |
| CFG LowIR, scoperta funzioni, jump table | Casi lift CFG/switch | `NeverDSwitchXformTests`, `NeverDCFGLoopXformTests` o `NeverDTwoTableXformTests` |
| MedIR, ABI, flag, tipi, SSA | Casi lift MedIR/convenzione di chiamata | Casi `NeverDSemanticTests` multi-ISA |
| HighIR o C strutturato | Casi HighIR/decompile | `NeverDCFGLoopXformTests` e compilazione del C generato |
| Loader PE/ELF/Mach-O o relocation input | Fixture formato corrispondente in `unittests/lift` | Test caricamento/decompilazione di tutte le fasi per la cella |
| Codegen di riscrittura o relocation output | Casi `RewriteCodegenRTTests` | `NeverDPatchFullTests` e fixture patch collegata se disponibile |
| Trasformazione LLVM IR usata da patch | Binario di trasformazione mirato | Griglia di pass composti `NeverDPatchFullTests` |
| C API o CLI | Test SDK/query diretto e `unittests/semantic/CLIEndToEndTests.cpp` | Suite pipeline/formato pertinente |
| Loader, opcode, IR o backend EVM | Target proprietario `NeverDEVM*Tests` più piccolo | Tutti i target EVM e compilazione del C/Solidity generato |
| Loader, ISA, IR o backend SBF | Target proprietario `NeverDSBF*Tests` più piccolo | Tutti i target SBF e compilazione del C/Rust generato |
| Riconoscimento libc | `NeverDLibCTests` | Casi semantici call/ABI se cambia il comportamento |
| Esecuzione o quoting di processi | `NeverDTestProcessTests` | Un caso CLI/semantico interessato su ogni host supportato |

I test devono esprimere il contratto al confine stabile più basso. Un test della
forma LowIR è utile per attribuire il lifter; serve un roundtrip semantico se due
forme IR plausibili possono comportarsi diversamente. Evita golden dump di
intere funzioni quando basta una piccola assertion su opcode, CFG o stato
osservabile.

## Relazione con la CI

La CI compila Release con test abilitati su Linux, macOS e Windows, controlla
l’inventario scoperto e poi applica esclusioni di label specifiche della
piattaforma. I profili sono in `.github/workflows/ci.yml` e
`scripts/audit_ci_test_inventory.py`. Poiché nessuno shard della matrice
rappresenta tutte le suite costose, un `check-neverd` locale resta il segnale
pre-merge completo più chiaro quando la macchina dispone di tutti gli strumenti
cross richiesti.

## Profilo corrente di conformità e sanitizer Solana SBF

Questa lista aggiornata sostituisce la lista SBF abbreviata precedente. La
suite source differential richiede `rustc` oltre a clang; uno skip del compiler
indica coverage mancante. L’aggregato completo include
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests` e `NeverDSBFSourceDifferentialTests`, oltre ai
target metadata, loader, analyzer, semantic, emitter e integration. Il profilo
integrato supera 107/107 casi in 13 binari.

Il profilo sanitizer viene costruito separatamente in `build-sbf-asan-ubsan`.
Supera 101/101 casi core in 12 binari senza report ASan o UBSan; integration
resta nella build LLVM integrata perché il package prebuilt omette l’header
fork-only richiesto.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF' -E 'SBFIntegration'
```
