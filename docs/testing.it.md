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
| `unittests/safety` | `NeverDSafetyTests`, `NeverDSafetyIntegrationTests` | Catalogo di sink, precedenza di identità, prefiltro degli argomenti, hunt di overflow di copia, audit di vita dell’heap e matrice obbligatoria a sei celle PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | Forme LowIR decoder/lifter, fasi IR, loader, relocation, fixture di formato, decompilazione e flussi patch rappresentativi |
| La maggior parte di `unittests/semantic` | `NeverDSemanticTests` | Semantica differenziale di istruzioni, ABI, controllo, espressioni C e lift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests`, `NeverDEVMBytecodeTests`, `NeverDEVMLoaderTests`, `NeverDEVMABITests`, `NeverDEVMAnalyzerTests`, `NeverDEVMDecoderPropertyTests`, `NeverDEVMProxyTests`, `NeverDEVMCallTests`, `NeverDEVMSemanticTests`, `NeverDEVMEmitterTests`, `NeverDEVMIntegrationTests` | Metadata hardfork, normalizzazione, ambiguità ABI/firma, CFG/SSA/recovery, confini decoder esaustivi e input ostili, fatti proxy/call, semantica interpreter, differenziali LLVM/C/Solidity e API pubblica |
| `unittests/sbf` | `NeverDSBFMetadataTests`, `NeverDSBFProgramImageTests`, `NeverDSBFLoaderTests`, `NeverDSBFAnalyzerTests`, `NeverDSBFVerifierTests`, `NeverDSBFISAConformanceTests`, `NeverDSBFAgaveConformanceTests`, `NeverDSBFSemanticTests`, `NeverDSBFEmitterTests`, `NeverDSBFLLVMEmitterTests`, `NeverDSBFLLVMDifferentialTests`, `NeverDSBFSourceDifferentialTests`, `NeverDSBFMalformedCorpusTests`, `NeverDSBFUpstreamConformanceTests`, `NeverDSBFExternalOracleTests`, `NeverDSBFSolanaModelTests`, `NeverDSBFIntegrationTests` | Metadati v0-v4 e layout ELF, comportamento rigoroso di verifier/loader, 23 artefatti ELF fissati, oracle ufficiale indipendente, disponibilità esaustiva degli opcode, input ostili, CFG/recupero e differenze eseguite LLVM/C/Rust |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | Equivalenza riscrittura/offuscamento su quattro ISA e tre formati oggetto |
| File di trasformazione mirati in `unittests/semantic` | `NeverDSwitchXformTests`, `NeverDIndCallXformTests`, `NeverDCFGLoopXformTests`, `NeverDTwoTableXformTests`, `NeverDAvxUpperXformTests` | Sonde veloci da ricollegare separate dal grande binario semantico |
| `unittests/corpus` (sottomodulo) | `NeverDWindowsEHCorpusTests`, `NeverDRustEHCorpusTests`, `NeverDGoEHCorpusTests`, `NeverDCxxItaniumEHCorpusTests`, `NeverDObjCEHCorpusTests` | Metadati di eccezioni e runtime letti da 317 binari reali fissati, ciascuno dichiarato in un manifest con le soglie minime che il suo recupero deve superare |

Le fonti autorevoli per la registrazione sono
[`unittests/CMakeLists.txt`](../unittests/CMakeLists.txt),
[`unittests/lift/CMakeLists.txt`](../unittests/lift/CMakeLists.txt) e
[`unittests/semantic/CMakeLists.txt`](../unittests/semantic/CMakeLists.txt),
[`unittests/evm/CMakeLists.txt`](../unittests/evm/CMakeLists.txt) e
[`unittests/sbf/CMakeLists.txt`](../unittests/sbf/CMakeLists.txt) e
[`unittests/safety/CMakeLists.txt`](../unittests/safety/CMakeLists.txt).

### Il corpus binario fissato

Ogni altra suite costruisce ciò che prova; il corpus no: è un sottomodulo di
binari prodotti da toolchain reali, su host e per target che questo repository
non può raggiungere. Ognuno è fissato per digest e accanto un manifest dichiara
le soglie minime che il suo recupero deve superare. È l’unico posto in cui
un’affermazione su ciò che NeverD legge da, poniamo, un oggetto condiviso
`armv7` compilato con `-O2` e privato dei simboli trova una risposta anziché una
discussione.

Le suite vengono costruite solo se al passo di configurazione è stato detto di
cercarle, quindi è quel flag a tenerle sotto test:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` esegue tutte le linee; `check-neverd-windows-eh-corpus`,
`check-neverd-rust-eh-corpus`, `check-neverd-go-eh-corpus`,
`check-neverd-cxx-itanium-eh-corpus` e `check-neverd-objc-eh-corpus` ne eseguono
una ciascuno. Tutti e tre gli host di CI configurano con il flag ed eseguono le
cinque linee: i byte sono identici ovunque, ma ciò che li legge non lo è, e una
passata del corpus su un host non prova nulla sugli altri due.
`scripts/audit_ci_test_inventory.py` rifiuta un inventario a cui manchi una
delle cinque etichette, perché una build che ha smesso in silenzio di leggere il
corpus è una regressione che nessun test può cogliere: il test è proprio ciò che
è sparito.

L’audit live degli opcode EVM si esegue così:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

In locale e in CI il percorso standard forza
`git fetch --depth=1 --force` sull’URL ufficiale
`https://github.com/ethereum/go-ethereum.git` e prova soltanto lo SHA esatto
appena ottenuto dal `HEAD` remoto del branch predefinito, in un worktree
detached. Ogni esecuzione usa un repository bare privato, temporaneo e
dal nome imprevedibile. Mantiene l’authority ref del fetch e il suo SHA esatto
per la vita del worktree detached, poi distrugge entrambi. Non esistono
repository Git persistenti o cache condivisi. `local_docs`, un checkout
esistente e un submodule non sono percorsi d’audit: un pin di submodule
diventerebbe obsoleto proprio quando serve rilevare il drift live.

Ogni comando Git rimuove prima tutti i `GIT_*` ereditati, inclusi
`GIT_CONFIG_*`, e poi installa soltanto valori revisionati.
`GIT_CONFIG_NOSYSTEM` e `GIT_CONFIG_GLOBAL` disabilitano la configurazione
system/global; `GIT_ATTR_NOSYSTEM` e `core.attributesFile` per comando
disabilitano gli attributi system/global, mentre `core.hooksPath` disabilita gli
hook. Configurazione inattesa del repository privato, grafts,
`objects/info/alternates` o `refs/replace` fanno fallire la validazione;
`GIT_NO_REPLACE_OBJECTS` disabilita replacement lookup.

Il probe riflette tutti i bool esportati di
`params.Rules`, chiama `LookupInstructionSet(params.Rules)` e scandisce tutti i
256 slot. `EVMUpstreamOpcodePolicy.def` possiede alias e typed exclusions
storiche/EOF non pianificate; `EVMUpstreamSemanticsPolicy.def` possiede
l’inventario Rules chiuso, mapping dei fork, eccezioni base-stack e famiglie
dynamic-immediate.

CI esegue lo stesso audit live soltanto sui push a `dev`, sulle pull request,
manualmente e ogni giorno. Il probe Go chiama l’API pubblica
`LookupInstructionSet(params.Rules)` per ogni fork mappato.
La CLI pubblica espone soltanto `--manifest-output`; il manifest chiuso usa
`schema 3` e non permette di scegliere source, ref, checkout o toolchain.
`EVMUpstreamOpcodePolicy.def` gestisce alias ed esclusioni storiche/EOF non pianificate
revisionate; l’ortogonale `EVMUpstreamSemanticsPolicy.def` gestisce regole dei
fork ed eccezioni della semantica dello stack. Il manifest chiuso verifica
revisione esatta, attivazione, byte/name, `base_min_stack` e `net_stack_delta`,
e rifiuta field, fork, nomi o byte sconosciuti o duplicati. L’allocazione usa
soltanto `operation.undefined`; `HasCost` serve soltanto da controllo incrociato
del costo perché vale false anche per le operazioni definite a costo zero. Ogni slot
`defined && !HasCost` deve corrispondere esattamente a
`EVM_GETH_ACTIVE_WITHOUT_COST` dal fork dichiarato. Uno slot undefined con costo,
uno defined non revisionato o la perdita del marker falliscono in modo chiuso.
Dichiarazioni mancanti, fuori range o non consumate sintatticamente falliscono
anch’esse: ogni `.def parser` rifiuta una policy `partial`. Un errore CI carica
revisione, manifest e log come artifact. Parser e diagnostic hanno copertura
unit Python indipendente:

`EVMUpstreamSemanticsPolicy.def` assegna ogni campo booleano esportato di
`params.Rules` a un solo `EVM_GETH_RULE_FIELD`: `MappedForkSelector`,
`NoOpcodeAllocation` o `ExcludedSelectorExpectedError`. Il probe abilita ogni
campo isolatamente tramite `LookupInstructionSet`: le prime due categorie
richiedono nil error, la terza error, e ogni fingerprint opcode/stack completo
di 256 slot deve essere `ExpectedFork`. `IsEIP155`, `IsEIP2929`, `IsEIP4762` e
`IsPetersburg` sono ora campi senza allocazione con fingerprint Frontier;
`IsUBT` deve fallire e produrre Cancun.

`EVMUpstreamSemanticsPolicy.def` dichiara le famiglie dinamiche EIP-8024, i tipi
di operazione e i delta stack validi; `EVMEIP8024Immediates.def` possiede
separatamente il decode degli immediate e classifica i 256 byte single/pair.
Con `go -overlay`, l’audit ottiene i veri handler privati `operation.execute` e
percorre una per una le `canonical fork jump tables` e le
`mainnet active/scheduled jump tables`. Registra una famiglia `inactive` e
rifiuta una `partial`. Ogni tabella attiva prova `DUPN`, `SWAPN` ed `EXCHANGE` su tutti gli
immediate (`3x256`) più i `3 missing-operand cases` rispetto alle stesse fonti
dichiarative.

`EVM_HARDFORK_LATEST` ha un solo target canonico. Il closed
`EVMUpstreamForkAliases.def` mappa Prague→Pectra, Osaka e BPO1–BPO5→Fusaka e
Paris/Shanghai/Cancun/Amsterdam/Bogota su se stessi; nomi ignoti falliscono
chiuso. Un `audit_unix_time` registrato guida
`MainnetChainConfig.LatestFork(time)` (deve eguagliare NeverD latest) e il check
alias/probe di `LatestFork(max uint64)`; entrambi gli instruction set sono
confrontati integralmente. Il manifest fissa `authority=official-fresh-fetch`,
URL ufficiale, `HEAD` richiesto e SHA. Il probe usa `GOTOOLCHAIN=local`.

Il probe Go e il controller Python applicano
`input/collection/string hard limits`; input, collection o stringhe
sovradimensionate falliscono in modo chiuso. Per `bounded diagnostic output`,
una visualizzazione troppo lunga include il `digest` completo e un
`explicit truncated marker`. Output e deadline limitati valgono per ogni child;
al superamento viene terminato l’intero `process group`/process tree e vengono
drenate le pipe.

La ricevuta schema 3 corrente registra `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revisione
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` locale,
`stack_limit=1024` e `diagnostics=[]`. Copre `21 fork tables` e
`20 Rules probes` con `15 mapped/4 no-op/1 expected-error`. Entrambi i record
`mainnet active/scheduled` riportano `upstream BPO2`, mappato in modo chiuso a
`NeverD Fusaka`. Dei `23 table targets`, soltanto `Amsterdam/Bogota` sono attivi:
`1536 candidate executions` e `6 missing-operand cases`. I
`three handler symbols` coincidono sui due target attivi. Audit Python `67/67` e
`C++ Opcode 10/10` sono passati. Il run macOS reale è riuscito sotto
`sandbox-exec`, con il `go run` finale offline; Linux richiede `bubblewrap`.

Tutte le fasi Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` e `go run` — attraversano il filesystem sandbox
`capability-root`. Legge solo probe privato, geth fresco, `resolved GOROOT`
validato e le precise root runtime di sistema necessarie, e scrive solo nelle
root isolate dell’ambiente. La rete è concessa soltanto alle fasi dependency
necessarie; il run finale è offline. I test esigono il rifiuto dei sentinels in
`host HOME/workspace` e l’assenza del loro contenuto dall’output. Linux verifica
la stessa policy `bubblewrap` senza `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

Gli undici target EVM attualmente registrati da CMake sono:

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` esaurisce tutti gli input di due byte per ogni
fork che cambia il decoder, confronta il decode completo e i confini `JUMPDEST`
esatti e passa input ostili deterministici di lunghezza limitata in tutti i fork.

Per modifiche al control flow EVM, esegui prima il contratto di punto fisso e
dominio delle altezze:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

Questi casi coprono return tra block, merge finiti multi-target, convergenza,
ordine deterministico, lane dell’intero stack sensibili al percorso,
correlazione preservata, jump sconosciuti, target esattamente non validi, budget
fail-loud e stack fault. `MayReachable` conserva solo un candidato CFG e non
produce fatti certi. Esegui poi tutti gli undici target EVM e l’audit live upstream.

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

### Fixture di sicurezza della memoria

`unittests/safety/fixtures/binaries` contiene immagini PE, ELF e Mach-O
versionate per x86-64 e AArch64, insieme al PDB o al dSYM che ciascun formato
fornisce e a un MAP del linker per ogni immagine. Il MAP è ciò che una build
spogliata continua a distribuire, quindi ogni cella viene analizzata anche
indicando il MAP in modo esplicito, il che fissa che cosa un risultato può
ancora affermare quando non restano né tipi né righe sorgente.
`NeverDSafetyIntegrationTests` esegue tutte e sei le celle su ogni host; la
configurazione fallisce se manca un’immagine o un file di accompagnamento
richiesto, e la suite non ha alcun percorso di salto legato alla toolchain
dell’host.

I binari equivalenti derivano da un unico file sorgente. Ricostruisci la fixture
smoke nativa dell’host con `make`, oppure rigenera l’intera matrice versionata
con:

```bash
make -C unittests/safety/fixtures matrix
```

La ricetta della matrice richiede i target incrociati Linux e Windows di Clang,
gli strumenti COFF di LLD, entrambe le architetture Darwin e `dsymutil`. I suoi
percorsi di debug vengono rimappati e la registrazione della riga di comando
CodeView è disattivata, così i file di accompagnamento versionati non catturano
il percorso assoluto dell’area di lavoro di chi sviluppa.

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

### Modelli di eccezioni per linguaggio

Tutto ciò che non è il modello tabellare di Windows sta in un unico target
mirato. `NeverDLanguageEHTests` copre la catena di frame DWARF, l'area dati
specifica del linguaggio di Itanium, ARM EHABI, il compact unwind di Darwin, i
metadati di frame del runtime Go, la macchineria di panic di Rust e i tre
runtime Objective-C:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

Le tabelle di questa suite sono assemblate byte per byte anziché compilate,
perché la maggior parte delle combinazioni da verificare non viene emessa
insieme da nessuna singola toolchain. Objective-C è il caso più netto: i tre
runtime emettono tutti una LSDA Itanium e differiscono solo per ciò che sta in
uno slot della tabella dei tipi, e quella differenza è totale, non di grado. Lo
slot di Apple indirizza un `objc_typeinfo` i cui primi due campi imitano
deliberatamente `std::type_info`; quello Objective-C++ di GNUstep indirizza una
vera sottoclasse di `std::type_info`; e quello del runtime GNU non è nemmeno un
puntatore, ma la stringa del nome della classe. Applicare la convenzione di un
runtime alla tabella di un altro non fallisce: riporta un nome di classe letto
dal mezzo di qualcos'altro. Per questo il runtime viene stabilito dalla
personality del frame prima di leggere qualsiasi slot.

La stessa suite fissa due distinzioni facili da confondere e sbagliate una volta
confuse. `@catch(id)` e `@catch(...)` sono gestori diversi — il primo accetta
qualsiasi oggetto Objective-C e lascia proseguire un'eccezione estranea — e ogni
runtime li scrive in modo diverso; un decodificatore che riporti entrambi come
catch-all mette un gestore su eccezioni che in realtà sarebbero passate oltre. E
una tabella di call site setjmp/longjmp indicizza i punti di chiamata invece
degli indirizzi: un lettore che non riconosca una delle personality SJLJ non
fallisce, ma inventa intervalli protetti e landing pad che il programma non ha
mai nominato.

Riconoscere quella forma non equivale a rifiutarla. Una voce SJLJ è una coppia
di valori ULEB128 — un selettore di dispatch e uno scostamento di azione — e
quello scostamento significa lì esattamente ciò che significa nella forma a
indirizzi: la catena di azioni, i tipi catturati e le specifiche di eccezione
si leggono quindi tutti da una tabella che non nomina alcun codice. Resta
ignota soltanto la regione che ciascuna voce protegge, perché a enunciarla sono
le scritture che la funzione stessa compie nel proprio slot di call-site, non
qualcosa nella tabella. La suite fissa anche l'unico byte di cui qui non ci si
deve fidare: GCC scrive `DW_EH_PE_uleb128` come codifica delle call-site e LLVM
scrive `DW_EH_PE_udata4`, entrambi emettono poi ULEB128 comunque, e nessuna
personality lo legge mai — quindi non deve leggerlo nemmeno un decodificatore.

L'identità della personality è fissata accanto a questo, perché è ciò che
decide come si legge ognuna delle tabelle qui sopra. GNAT nomina la propria
routine nei tre modi in cui GCC nomina quella di ogni frontend — `_v0`, `_sj0`,
`_seh0` — e su Windows registra un simbolo mentre inoltra a un altro, così
tutte e quattro le grafie devono ricadere su Ada. D ne è l'immagine speculare:
tre compilatori, tre nomi per una sola routine, un unico insieme di tabelle
alle spalle.

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

I test Low/Med conservano execution lane whole-stack sensibili al path e
l’identità della lane nei phi; esaurire un budget, incluso
`MaxAbstractInstructionTransfers`, è un hard error. Strict rifiuta un opcode
ignoto o inattivo soltanto su una lane provata `Reachable`; `MayReachable` non
produce fatti definitivi. HighIR vincola selector, receive e fallback alla lane
radice e a terminali riusciti. Un selector condiviso non è evidenza indipendente
di standard: solo una `KnownFunctionVariantInfo` dello standard e una forma di
ritorno esatta concordata da tutti i terminali riusciti consentono di scegliere
variante e lista dei ritorni.

L’interpreter esegue il preflight tipizzato dello stack prima di ogni effetto
specifico dell’opcode. `EVMForkSemantics.def` definisce il byte `0x44` come
`DIFFICULTY` prima di Paris e `PREVRANDAO` da Paris. `REVERT`, fault, step limit
ed esaurimento delle risorse ripristinano lo stato della transazione. Un errore
di allocazione è `ExecutionFaultKind::ResourceExhausted`; se non si può creare
lo snapshot d’ingresso, `HasPersistentStateSnapshot` è false e il risultato non
può essere committed.

### Regressioni dei confini pubblici e dei budget EVM

I test delle API pubbliche alterano separatamente
`Code`/`Fork`/`Instructions`/`JumpDestinations` canonici e ogni tabella, range,
ID, lane e riferimento edge di LowIR. `execute` deve restituire `llvm::Error`
prima del lookup delle istruzioni; `lowerToMedIR` deve rifiutare tutto il LowIR
malformed o fuori budget prima di costruire indici o allocare output
proporzionale all’input. Per `lowerToMedIR`, i test impongono validation di
options, risorse e struttura prima del `canonical decode replay` field-by-field
e prima di `lowerCanonicalLowToMedIR`. Il recovery HighIR pubblico replay-verifica
LowIR/MedIR esterni; soltanto `analyze` usa `lowerCanonicalLowToMedIR` e
`recoverCanonicalHighIR` sul proprio IR canonico senza replay ricorsivo o
duplicato, ma con tutti gli HighIR option/resource budgets. L’interpreter prova poi il confine esatto e +1 per ogni
limite di `EVMInterpreterLimits.def`: `MaxSteps` mantiene il suo `StepLimit`;
l’esaurimento di `MaxMemoryBytes`, `MaxTraceEntries`, `MaxLogEntries`,
dell’aggregato `MaxLogDataBytes` o di `MaxPersistentStateEntries` runtime
restituisce `ResourceExhausted` e ripristina gli effetti transazionali. Un
aggregato iniziale `MaxHostReturnDataBytes` o persistent state troppo grande è
un errore API. Anche `MaxCalldataBytes`, l’aggregato
`MaxHostEnvironmentEntries` su `BlockHashes`, `Balances`, `CodeHashes`,
`ExternalCode`, `BlobHashes` e l’aggregato `MaxExternalCodeBytes` sono errori
API. Il `const execute preflight` li rifiuta prima di copiare environment,
snapshot o result. Sono coperti anche view return-data `ArrayRef` e lookup
`lower_bound` su tabella ordinata, senza copia di buffer né PC map.

Test LowIR separati coprono i limiti diagnostic aggregati `MaxLowDiagnostics` e
`MaxLowDiagnosticBytes`: decode lineare e costruzione CFG preaddebitano count/
byte finali esatti e rifiutano zero.
I test di sicurezza HighIR coprono il dominio ordinato per lane
`Any/Exact/Excluded`, match/esclusione dell’uguaglianza, false-edge match e
true-edge mismatch di un `XOR(selector, constant)` grezzo, raffinamento di word
zero/calldata size/call value e condizioni unknown fail-closed. I test al confine
esatto e -1 coprono, da `EVMAnalysisLimits.def`,
`MaxHighDispatchCandidates`, l’aggregato `MaxHighRecoveredArguments`,
`MaxHighDiagnostics`, `MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` e `MaxHighMemoryValueVisits`. Ogni diagnostic
emesso, incluso quello fisso per malformed, deve addebitare count e byte finali
prima dell’allocazione.
I budget diagnostic LowIR e HighIR sono testati separatamente; la root CFG
region predefinita deve addebitare `MaxHighRegionBlockReferences` prima di
reserve o copia dei block PC.
Le regressioni di function scope coprono i back-jump `EQ` e `raw XOR` al
dispatcher condiviso. Verificano che un’altra funzione non contamini
`arguments`, `mutability`, `return shape` o `region`, mantenendo raggiungibili
body condivisi e tail call.
Gli esiti esterni CALL/CREATE sono provati come outcome host non deterministici
su entrambi gli edge CFG precisi, preservando la recovery del fallback ERC-1167.
Una condizione selector illeggibile resta Unknown e non può inventare fatti
fallback o function.

I test CFG derivano `InvalidJumpDestination` da `EVMLowFaultKinds.def` per un
`end-of-code JUMPI`: true certo verso un target invalido non ha coda di successo
ed è un fault certo; false certo ha successo; unknown conserva il possibile
percorso false di successo senza marcare tutta la lane come fault certo.

I test ABI applicano al limite esatto e +1 i confini grammaticali di
`EVMABIParserLimits.def` e quelli di cardinalità/testo delle tabelle pubbliche
di `EVMABITableLimits.def`. Rifiutano inoltre enum kind/standard/evidence
invalidi, metadata incoerente, signature/return non canonici, selector condivisi
marcati erroneamente independent, variant dangling o duplicate e un event-topic
`APInt` di width non-word prima del lookup selector indicizzato o del lookup
topic ordinato.

`NeverDEVMOpcodeTests` impone anche l’architettura metadata: ogni opcode assegnato
fa roundtrip tra encoding e valore tipizzato; vengono testati confini di famiglia,
alias hardfork e massimi stack/host derivati.

### Backend differenziali Solana SBF

I test dei metadati SBF convalidano ogni funzionalità di versione, i confini di collisione degli opcode, gli hash syscall Murmur3, le rilocazioni e le costanti di machine ELF, registro e indirizzo VM. Le fixture del loader generano, senza binari inclusi, sia layout legacy v0-v2 a sezioni sia layout rigorosi v3/v4 senza sezioni e basati sui program header.

`NeverDSBFISAConformanceTests` verifica ogni byte encoding per ciascuna versione
v0-v4 rispetto a un manifest tipizzato sottoposto ad audit indipendente.
`NeverDSBFExternalOracleTests` confronta poi le decisioni di attivazione e di
confine con un processo Anza ufficiale costruito separatamente.
`NeverDSBFUpstreamConformanceTests` assegna un esito esplicito a tutti i 23 ELF
alla revisione Anza fissata.

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
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# Tutti i target/casi Solana SBF mirati
cmake --build build-release --target check-neverd-sbf --parallel 4
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
| Audit di vita dell’heap o hunt di overflow di copia | `NeverDSafetyTests` | Tutte le sei celle di `NeverDSafetyIntegrationTests` |
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
`scripts/audit_ci_test_inventory.py`. `NeverDSafetyTests` e
`NeverDSafetyIntegrationTests` sono obbligatori su ogni host della matrice;
ogni esecuzione legge le stesse fixture PE, ELF e Mach-O versionate per x86-64
e AArch64. Poiché nessuno shard della matrice rappresenta tutte le suite
costose, un `check-neverd` locale resta il segnale pre-merge completo più chiaro
quando la macchina dispone di tutti gli strumenti cross richiesti.

## Profilo corrente di conformità e sanitizer Solana SBF

Questa lista aggiornata sostituisce la lista SBF abbreviata precedente. La
suite source differential richiede `rustc` oltre a clang; uno skip del compiler
indica coverage mancante. L’aggregato completo include
`NeverDSBFProgramImageTests`, `NeverDSBFMalformedCorpusTests`,
`NeverDSBFISAConformanceTests`, `NeverDSBFUpstreamConformanceTests`,
`NeverDSBFLLVMDifferentialTests` e `NeverDSBFSourceDifferentialTests`, oltre ai
target metadata, loader, analyzer, semantic, emitter e integration. Il profilo
integrato registra target e risultati nominati, non un totale che cambia presto.

Il profilo sanitizer viene costruito separatamente in `build-sbf-asan-ubsan`.
Il package prebuilt fissato per revisione include l’header fork-only richiesto,
quindi integration gira nello stesso profilo ASan/UBSan fail-fast.

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### Snapshot di evidenza SBF fissato (2026-08-24)

La gate fissa Anza `sbpf` a
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave a
`ef210d67f2fabeee1730498188fa78854260c679` e Solana SDK a
`122f32e571ce39face4beffaccea733e37c207fd`. Il manifest ELF ufficiale passa
23/23; `NeverDSBFExternalOracleTests` confronta 1,411 casi opcode/boundary via
`SBFOfficialOracleProtocol.def`, `SBFOfficialVerifierCases.def` e
`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` è il contratto tabellare degli ELF malformati; il
totale variabile non viene fissato.
Separatamente, il `41-case strict ELF differential` esegue l’intera matrice
strict-v3 tramite `verify-elf-batch` ufficiale e NeverD; i suoi 41 casi non
fanno parte del totale 1,411.

La matrice di esecuzione ufficiale aggiuntiva resta separata: esattamente 508
casi attivi `(Version,Opcode)` più 58 casi di confine producono 566 casi di
esecuzione esatta. Non sostituisce né rientra nelle 1,411 probe del verifier o
nel `41-case strict ELF differential`.
`NeverDSBFAgaveConformanceTests` autentica Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862` e confronta tutti i 1,955 `sol_compat_elf_loader_v1` fixture
del loader (1,399 accettati, 556 rifiutati). Per ogni ELF accettato confronta
`entry_pc`, `text_off`, `text_cnt`, `rodata_hash` e `calldests_hash`. Questa gate non esegue il successivo
instruction verifier.
La Linux Release CI usa `--print-pinned-revision`,
`--print-test-vectors-revision` e `--print-toolchain`, ed esporta
`NEVERD_SBPF_ORACLE` e `NEVERD_AGAVE_CONFORMANCE_ROOT`, rendendo obbligatorie
entrambe le gate esterne. In locale, senza env oracle/corpus esplicito, i casi
vengono scoperti ma possono essere saltati.

`SBF_RUNTIME_VERSION` rende `RuntimeVersionPolicy::ChainProfile` dipendente dal
cluster/slot storico: i feature account ufficiali fanno avanzare l’ISA massimo
da V0 a V1, V2 e V3; oggi resta V3. v4 esplicito usa
`RuntimeVersionPolicy::UpstreamToolchain` per analisi
offline. Il limite corrente di 10 MiB è esattamente `10'485'760` byte; 65,536 è
solo provenance/test storico. `SBFFaultCodes.def` stabilizza i valori degli
execution fault e `SBFSourceStatuses.def` possiede separatamente l’ABI source.

Fixture in scala 10,000 proteggono worklist, function ownership e multi-latch
senza fissare tempi di macchina. Le righe cluster/account/slot consentono un
`RPC activation audit` mentre i test ordinari restano deterministic e offline.
