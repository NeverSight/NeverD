**Lingue**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Decompilazione EVM

[← Indice della documentazione](README.it.md)

NeverD carica il bytecode tradizionale dell’Ethereum Virtual Machine, costruisce
LowIR dedicato a 256 bit, MedIR stack-SSA e HighIR recuperato, quindi emette LLVM
IR, C23 o Solidity. L’analisi strict è predefinita, ma EVM legacy non valida gli
opcode dell’intera immagine: viene rifiutata soltanto una lane di esecuzione
sicuramente `Reachable` che raggiunge un opcode non assegnato o inattivo per il
fork, al suo PC esatto. Byte morti e candidati CFG solo `MayReachable` non
diventano errori strict.

Gli output Solidity e C sono ricostruzioni semantiche. Conservano ordine degli
opcode, aritmetica a 256 bit, controlli dello stack e control flow validato, ma
non dichiarano di riprodurre sorgente, identificatori o tipi originali.

## Avvio rapido

```bash
# LLVM IR verificato con valori i256/i512.
./build/bin/neverd lift contract.evm -o contract.ll

# Ispeziona ogni stadio di analisi EVM.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# Emetti C23 o Solidity.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Seleziona opcode storici o conserva quelli ignoti come nodi di fault.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg` e le query Low/Med/High/LLVM della C API accettano EVM. Il
binary rewriting EVM è rifiutato esplicitamente; `patch` resta un’operazione nativa.

## Input accettati

| Input | Riconoscimento e normalizzazione |
|-------|----------------------------------|
| Byte grezzi | `.raw`, `.evmraw` o contenuto binario con estensione EVM esplicita |
| Testo esadecimale | `0x` opzionale, whitespace ASCII arbitrario, `.evm`, `.hex`, `.bin`, `.bytecode`; viene rilevato anche hex senza estensione dopo la validazione |
| Artefatto del compilatore | `.json` con `deployedBytecode`, `runtimeBytecode` o `bytecode` alla radice o sotto `evm`; supportato anche JSON standard solc `contracts → file → contract → evm` |

Il bytecode runtime/deployed è preferito al creation bytecode. Se è presente
solo creation code, NeverD riconosce wrapper `CODECOPY`/`RETURN` costanti e
limitati ed estrae la slice runtime copiata. La visita del constructor usa lo
stesso decoder a singola istruzione del decoder reale, sotto l’hardfork in
analisi, così un byte che su un fork è dato e su un altro è opcode non può
spostare il confine. Un field `deployedBytecode` o `runtimeBytecode` presente è
autorevole: il valore esplicito `0x` è accettato come runtime vuoto che termina
naturalmente e impedisce intenzionalmente il fallback al creation bytecode. Un
field assente può passare al candidato successivo; hex mancante o composto solo
da whitespace senza prefisso esplicito viene rifiutato. Anche l’input raw
esplicito può essere vuoto.

### Trailer del compilatore

`EVMMetadataFields.def` tabula entrambi i formati di trailer. Solidity scrive
una mappa CBOR i cui due byte finali contano la sola mappa; `vyper` scrive un
array CBOR che termina con quella mappa, e i suoi due byte finali contano
l’intero footer, se stessi inclusi. Leggere un framing come se fosse l’altro non
fallisce in modo rumoroso — atterra due byte più in là e rimuove due byte di
codice reale — perciò si tentano entrambi e un input che non corrisponde a
nessuno dei due viene lasciato intatto.

Il trailer viene letto due volte: una sull’input così com’è e una sul runtime
code che resta dopo aver rimosso un wrapper di deploy. Vyper ha spostato il
proprio trailer nell’initcode e lascia il runtime code senza, quindi un lettore
che guarda solo dopo l’unwrap riporta una build ignota per un contratto che si
era dichiarato. Un footer di sequenza indica anche la lunghezza del runtime
code, quelle delle data section e quella degli immutable, che limitano il codice
restituito senza eseguire il constructor.

### Container che non sono istruzioni

`EVMBytecodeContainers.def` classifica l’input prima di qualsiasi decodifica.
Da quando EIP-3541 ha reso `0xEF` non deployabile, un `0xEF` iniziale promette
che quei byte non sono istruzioni:

| Container | Marker | Trattamento |
|-----------|--------|-------------|
| legacy | — | decodificato come istruzioni |
| delegation (`eip-7702`) | `0xef0100` ed esattamente 23 byte | riporta l’account di destinazione; l’analisi si ferma |
| eof (`eip-3540`) | `0xef00` | rifiutato; nessun fork lo ha attivato |

I venti byte di un delegation indicator sono un indirizzo, non codice.
Decodificarli leggerebbe l’indirizzo come opcode e produrrebbe il control-flow
graph di un account, perciò `info` riporta la destinazione e l’analisi rifiuta
indicandone il motivo. Il rifiuto distingue i due casi: prima di Pectra il
marker non è ancora assegnato, e da Pectra in poi manca semplicemente il runtime
code della destinazione. Un marker con qualunque altra lunghezza è input
malformato e non una variante del container, quindi resta istruzioni perché il
decoder possa nominare il byte che non è riuscito a leggere.

Hex malformato, cifre dispari, placeholder linker irrisolti, artefatti
multi-contract ambigui, limiti metadata invalidi e hex mancante o vuoto
producono errori utilizzabili. Un input raw esplicitamente vuoto o un runtime
`0x` resta invece un programma vuoto valido.
`BytecodeLoadOptions::ArtifactContract` seleziona `Contract` o
`path/File.sol:Contract`. Se più source file definiscono lo stesso nome, quello
non qualificato è rifiutato per evitare selezioni dipendenti dall’ordine JSON.

EVM è registrato nel core loader registry, non nascosto in un backend plugin.
CLI, C API, disassembler, CFG e query IR usano quindi la stessa image normalizzata
e le stesse opzioni.

## Hardfork e opcode

È coperto l’insieme di opcode legacy finalizzato da Frontier a Fusaka, inclusi
`PUSH0`, transient storage, `MCOPY`, opcode blob e `CLZ`. Gli opcode pianificati
per Amsterdam sono disponibili soltanto dietro un target di sviluppo esplicito;
`latest` resta Fusaka.

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

Sono accettati `dao`, forme con underscore, `merge`, `prague` e `osaka`.
Attualmente `latest` e `osaka` risolvono alla revisione canonica `fusaka`.

`latest` indica la revisione mainnet finalizzata più recente implementata, non
la testa di sviluppo Ethereum. [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/)
è previsto per Q4 2026; le istruzioni ancora in Review
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843) e
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) si attivano solo
con `--evm-hardfork=amsterdam` (o `bogota`) e restano fuori da `latest` fino
alla finalizzazione. In EIP-8024 viene consumato solo un immediate valido; un
candidato non valido rimane l'istruzione successiva.

EOF è stato rimosso nel
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 non è pianificato e la proposta di container
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) è Stagnant. Il vecchio
repository `execution-spec-tests` è archiviato e i test mantenuti sono migrati in
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
NeverD non presenta un container EOF sperimentale come comportamento mainnet.

Strict rifiuta un byte ignoto o inattivo soltanto quando una lane sicuramente
`Reachable` prova che l’esecuzione lo raggiunge. `--evm-relaxed` lo conserva come
prefisso di fault tipizzato e nei diagnostics, ma i backend generano fault se
viene eseguito; non diventa mai silenziosamente NOP.

## Architettura metadata in stile LLVM

I metadata EVM mantenuti a mano seguono il pattern `.def` multi-incluso di LLVM:

- `EVMOpcodes.def` è l’unica fonte per ogni opcode legacy finalizzato e per gli
  opcode di sviluppo opt-in. Encoding, mutazioni pop/push reali, tipo di immediate,
  class, activation fork, primary effect, accesso
  ortogonale a memoria, stato e call-value, e termination sono nello stesso
  record; non esistono default impliciti.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def` e
  `EVMCallValueAccesses.def` definiscono domini chiusi e tipizzati. `CALL` può
  essere external call e memory read/write; `EXTCODECOPY` context read e memory
  write. Lo stato usa la lattice `None/Read/Write/Unknown`. Payability è
  indipendente: `CALLVALUE` normalmente implica `payable`; viene soppresso solo
  se l’analyzer prova il guard canonico `ISZERO(CALLVALUE)` con ramo nonzero che
  termina in `REVERT`.
- `EVMImmediateKinds.def` definisce dati PUSH a larghezza fissa e gli encoding
  single/pair condizionali di EIP-8024; `EVMDecodeStatuses.def` possiede il
  vocabolario stabile esposto da LowIR e disassembly.
  `EVMUpstreamOpcodePolicy.def` registra l’alias di nome go-ethereum e le
  esclusioni storiche e di EOF non pianificate intenzionali. L’ortogonale
  `EVMUpstreamSemanticsPolicy.def` mappa i fork a `params.Rules`, nomina le
  eccezioni del precheck base dello stack e classifica le famiglie con immediate
  dinamico. L’audit rifiuta drift di byte, attivazione, `base_min_stack` e
  `net_stack_delta` e ogni nuova costante upstream non revisionata.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` e
  `OutputLanguages.def` generano enum ordinate, parser, nomi, scelte CLI e
  valori C ABI. `EVMAnalysisLimits.def`, `EVMInterpreterLimits.def`,
  `EVMABIParserLimits.def` e `EVMABITableLimits.def` dichiarano i limiti per
  stadio di analisi, interpreter, parser e tabelle pubbliche. `EVMConstants.h`
  centralizza le width di protocollo condivise e i nomi interni stabili, e
  materializza da `EVMAnalysisLimits.def` i default dell’analisi e i nomi delle
  opzioni diagnostiche; gli header dell’interpreter e dell’ABI materializzano i
  limiti delle rispettive tabelle.
- `EVMCalls.def` descrive le quattro istruzioni che chiamano un altro programma
  e il reticolo delle provenienze di un indirizzo di callee. Un solo flag per
  record, cioè se un operando di value sta fra il callee e la finestra degli
  argomenti, deriva ogni posizione successiva, e la tabella è validata contro il
  database degli opcode perché la derivazione non possa divergere dai pop
  dichiarati.
- `EVMPrecompiles.def` è il dizionario degli indirizzi a cui risponde il
  protocollo stesso, ciascuno con il fork che lo ha riservato e la proposta che
  lo ha pianificato. `P256VERIFY` a `0x100` è attribuita a `eip-7951`, la
  proposta Final che l’ha riservata su mainnet con Fusaka; la proposta rollup da
  cui proviene la sua interfaccia non l’ha mai pianificata. Il gas è assente
  di proposito: il costo di una precompile è funzione del suo input ed è stato
  riprezzato senza che indirizzo od operazione cambiassero.
- `EVMMetadataFields.def` e `EVMBytecodeContainers.def` descrivono che cosa sia
  un input prima che venga decodificato: i due framing di trailer del
  compilatore e i container i cui byte non sono affatto istruzioni.
- `EVMRecoveredFacts.def` possiede le grafie dei vocabolari dei fatti
  recuperati, così un nome che raggiunge l’output vive in un unico posto invece
  che in uno `switch` da cui un nuovo enumeratore può restare fuori.
  `EVMKnownSignatures.def` memorizza una sola volta spelling e selector canonici
  di ogni funzione, quindi dichiara membership per standard separate in
  `KnownFunctionVariantInfo`, con liste di ritorno e ruolo di evidenza
  independent/non-independent. Uno spelling condiviso da ERC-20/ERC-721 resta
  così un solo candidato invocabile, ma non prova da solo alcuno standard e non
  eredita il ritorno della prima variante. Eventi e custom error restano record
  tipizzati distinti.
- `Semantics.h` contiene il scalar ALU evaluator target-independent. Interpreter
  e constant folding condividono lo stesso `APInt` verificato; i lowering
  LLVM/C/Solidity restano espliciti e fail-loud.

Il decoder è il confine raw-byte. Identità assegnata e fork activation sono
separate: relaxed conserva nome, fork d’introduzione e immediate width di un
opcode inattivo, con semantic query conservativa che produce fault. Così un
immediate inattivo non sposta i confini successivi. Analisi, interpreter ed
emitter usano `Opcode` generato e metadata query; raw encoding riappare solo nei
confini ABI di trace/host callback. I 17 input stack di `SWAP16` e i 7 argomenti
host massimi sono limiti separati derivati a compile time.

`OpcodeInfo` non può essere default-constructed in uno stato semi-valido e il
nome è `llvm::StringLiteral`. Il validator compile-time rifiuta encoding duplicati,
property ignote, contratti ALU, mismatch effect/state, famiglie PUSH/DUP/SWAP/LOG,
terminator e risultati host invalidi. Solo una factory esplicita crea metadata
unknown conservativa.

I `.def` sono database scritti a mano come
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
`.inc` è riservato a fragment realmente generati, ad esempio da TableGen. I
record dichiarativi ricchi vivono in `.td` e
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) genera `.inc`. NeverD non
ha ancora questo step per EVM, quindi un `.inc` senza generator sarebbe solo
cerimonia. Il C++ segue gli [standard LLVM](https://llvm.org/docs/CodingStandards.html),
ADT/string LLVM ai boundary e switch semantici esaustivi.

Un nuovo opcode richiede record `EVM_OPCODE` completo, scalar semantics,
backend lowering espliciti e test focalizzati. Un hardfork richiede un
`EVM_HARDFORK` ordinato e alias. API tipizzata, lookup, validation,
classification e CLI crescono senza tabelle parallele.

## Modello di analisi

- **EVM LowIR** conserva PC, encoding, stato tipizzato dell’immediato e operandi
  decodificati di profondità dello stack (inclusi il right-zero padding di PUSH
  e la regola di consumo condizionale EIP-8024), block, edge predecessori/
  successori, target `JUMPDEST` validati, reachability e domini di stack height.
  Il recupero del CFG è un punto fisso deterministico sull’intero programma: per
  ogni stack slot viene propagato un insieme finito e limitato di valori a 256
  bit e per ogni altezza concreta viene conservato uno stack astratto. Le
  costanti trasportate attraverso block di internal call/return, stack shuffle,
  `PC`/`CODESIZE` e operazioni ALU scalari possono quindi risolvere uno o più
  target concreti. Un target realmente sconosciuto resta un edge indiretto
  esplicito anziché essere indovinato.

  Su un back-edge, uno slot loop-carried modificato è sovra-approssimato
  semanticamente a `Top` perché il punto fisso converga; questa astrazione di
  ricorrenza è indipendente dalle risorse. `MaxAbstractValuesPerSlot`,
  `MaxStackHeightVariants`, `MaxAbstractInstructionTransfers` e i limiti di
  istruzioni, block, stati, valori, stack, lane, edge e worklist sono budget con
  nome. Zero o esaurimento sono un hard error prima
  dell’inserimento, mai `emergency widening` o troncamento silenzioso.

  `EVMLowFaultKinds.def::InvalidJumpDestination` resta path-sensitive su un
  `end-of-code JUMPI`: una condizione sicuramente true verso un target invalido
  non ha coda di successo e registra un fault certo; una condizione sicuramente
  false ha successo. Unknown conserva soltanto il possibile percorso false di
  successo senza marcare erroneamente tutta la lane come fault certo.
- **EVM MedIR** rappresenta ogni valore dello stack come SSA a 256 bit e collega
  tutti i merge phi prima di eseguire una sparse constant worklist deterministica.
  Il lattice privato è `Uninitialized`, una `Constant` esatta oppure
  `Overdefined`: costanti uguali si propagano tra block e cicli phi ancorati,
  mentre un ciclo in conflitto o dipendente dal runtime non può inventare una
  costante. La worklist controlla gli ID def-use e usa lo stesso valutatore ALU
  di `Semantics.h` dell’interpreter. MedIR conserva anche l’effect semantico
  primario più, ortogonalmente, l’accesso EVM-memory
  `none/read/write/readwrite`, lo state access a livello sorgente e il call-value
  access. Ogni lane dello stack completo LowIR conserva una lane SSA di
  esecuzione distinta e i phi indicano la lane sorgente; stack incompatibili non
  sono allineati usando l’altezza massima.
- **EVM HighIR** recupera i selector del dispatcher Solidity, probabili parole
  calldata e return, mutability, storage slot costanti, fatti LOG/event e revert
  e regioni function/CFG. Un producer index controllato e un value walk
  iterativo con memoization recuperano i fatti dagli operandi tipizzati MedIR,
  non dalla distanza fra istruzioni: i confronti dei selector possono
  attraversare block e phi, usare entrambi gli ordini degli operandi `EQ` e
  conservare una maschera derivata a 32 bit; argument offset, storage key, event
  topic0, guard non-payable/receive e dimensioni return esatte di 32 byte usano
  i propri input semantici. Il grafo MedIR limita strutturalmente il walk, che
  considera unknown le espressioni malformed, miste o cicliche. Target in
  conflitto per lo stesso selector sono diagnosticati e omessi. Payability resta
  indipendente dallo state-access lattice e un dynamic jump raggiungibile non
  risolto impone un recupero `nonpayable` conservativo. Il dataflow memoria
  byte-granular segue write a offset costante fra block, compone overlap/kill e
  invalida la conoscenza con write dinamiche o sconosciute. Le prove sul payload
  coprono oggi selector e byte Panic noti. Per una dichiarazione custom error
  nota, l’emitter Solidity conserva i tipi canonici dei parametri; non afferma di
  recuperare ogni valore runtime degli argomenti. Gli altri fatti restano
  candidati sostenuti dalle evidenze.

  La scoperta dei selector parte soltanto dalla lane radice e segue gli edge di
  mancata corrispondenza del dispatcher: un confronto simile a un selector dentro
  un handler non diventa una funzione pubblica. Anche receive e fallback sono
  vincolati alla radice e richiedono un terminale riuscito sicuramente
  raggiungibile; revert, fault, un handler empty-calldata non pagabile o un path
  solo possibile non li dimostrano. Un uso incompatibile di calldata elimina il
  candidato canonico e un selector condiviso non è evidenza indipendente di uno
  standard. Solo selector indipendenti compatibili sufficienti o una prova forte
  di topic/arity esatti, storage slot o proxy possono scegliere standard e
  variante. Una lista di ritorni statici viene emessa solo se tutti i terminali
  riusciti sicuramente raggiungibili concordano sull’esatta lunghezza ABI;
  trasferimenti irrisolti, forme in conflitto o mismatch falliscono chiusi.
  Revert e fault non sono ritorni riusciti.

  HighIR applica budget separati a funzioni, visite lane/operazione, riferimenti
  di block nelle regioni, richieste e byte di memoria, celle di stato e update
  della worklist. Il punto fisso della memoria consuma soltanto lane di esecuzione
  sicuramente raggiungibili, fa meet per consenso byte per byte e restituisce un
  hard error all’esaurimento, senza troncare fatti.

  HighIR registra anche la metà uscente dell’interfaccia: ogni `CALL`,
  `CALLCODE`, `DELEGATECALL` e `STATICCALL`, con la provenienza del callee,
  l’indirizzo riservato che nomina quando il fork analizzato ne riserva uno, il
  selector che la chiamata pone in testa al calldata del callee e il valore
  trasferito quando è costante. `CREATE` e `CREATE2` sono esclusi perché
  eseguono codice che non ha ancora un indirizzo, quindi non c’è alcun callee da
  recuperare.

  Una firma uscente recuperata non entra mai fra gli standard a cui il programma
  risponde. Inviare `transfer(address,uint256)` dice che il programma usa un
  token, non che lo sia, e confondere le due cose segnalerebbe ogni router e
  ogni vault come ERC-20. Una chiamata delegante è inoltre segnalata come fatto
  di proxy, perché è l’unico membro della famiglia il cui callee esegue sullo
  storage di questo stesso programma.

  La ricerca delle precompile è vincolata al fork analizzato, non al più recente
  esistente. Chiamare l’indirizzo di una precompile introdotta da un fork
  successivo raggiunge un account senza codice, riesce e non restituisce nulla:
  nominarla segnalerebbe un’operazione che il programma dimostrabilmente non ha
  eseguito.
- **LLVM** emette una state machine `i32 @evm_execute(ptr)` verifier-clean con
  stack controllato di 1024 parole `i256`, intermedi `i512`, signed division
  protetta, shift saturi, `BYTE`/`SIGNEXTEND`/`CLZ` esatti e switch validati.

L’interpreter deterministico è l’oracle semantico. LLVM/C vengono compilati e
confrontati; Solidity viene deployato in Anvil e confrontato su storage e trace.
Un corpus raw pre-Fusaka gira anche nell’EVM nativa di Anvil per validare in modo
indipendente ALU, calldata copy, `MCOPY` sovrapposto, memory expansion, Keccak e
return data. Gli operand account sono mascherati a 160 bit secondo la
[specifica](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py),
le width ambientali sono validate e `BLOCKHASH` rispetta 256 block. Il buffer
EIP-211 è separato dall’output finale: solo `RETURN`/`REVERT` impostano
`ExecutionResult::ReturnData`; CREATE/CREATE2 seguono la stessa regola.

Prima di qualsiasi effetto specifico dell’opcode, l’interpreter controlla in
preflight l’altezza richiesta tipizzata, i pop e l’altezza conservata più i push:
underflow e overflow non possono eseguire mezza istruzione.
`EVMForkSemantics.def` seleziona il byte `0x44`: `DIFFICULTY` prima di Paris e
`PREVRANDAO` da Paris. `REVERT`, fault semantici, step limit ed esaurimento di
risorse per allocation/length ripristinano storage, transient storage, log ed
effetti selfdestruct allo snapshot d’ingresso, conservando diagnostica del frame
e byte espliciti del revert. Un errore di allocazione è marcato
`ExecutionFaultKind::ResourceExhausted` senza allocare una stringa d’errore; se
non è stato possibile creare nemmeno lo snapshot,
`HasPersistentStateSnapshot` è false e il risultato non è committable.

### Confini pubblici dell’IR e delle risorse

La funzione pubblica `execute` verifica prima che
`Code`/`Fork`/`Instructions`/`JumpDestinations` formino LowIR canonico. Un fork
alterato, un record di istruzione contraffatto, un encoding incoerente o una
tabella di destinazioni errata restituisce quindi `llvm::Error` prima che
l’interpreter indicizzi la tabella delle istruzioni. Il `lowerToMedIR` pubblico
valida nell’ordine options, risorse e struttura; poi un
`canonical decode replay` decodifica `Low.Code` con fork/strictness incorporati
e confronta ogni campo LowIR. Solo allora può chiamare
`lowerCanonicalLowToMedIR`, creare indici o allocare output proporzionale al
caller. Il `recoverHighIR` pubblico replay-valida allo stesso modo LowIR/MedIR
esterni. I percorsi privati `lowerCanonicalLowToMedIR` e
`recoverCanonicalHighIR` sono riservati all’IR posseduto da `analyze`: saltano
solo il replay ridondante non ricorsivo, mantenendo obbligatori tutti gli HighIR
option/resource budgets.

La prova del dispatcher conserva per ogni `MedStateLane` un dominio selector
ordinato `Any/Exact/Excluded`. I join uniscono i set Exact, intersecano le
esclusioni Excluded e sottraggono un set Exact da un’esclusione cofinita;
l’allargamento del dominio rimette la lane in coda. Un’uguaglianza registra il
candidato del true edge solo quando il selector è ancora consentito e lo
esclude sul false edge. Un `XOR(selector, constant)` grezzo registra lo
zero/false edge come match quando tutti i successor canonici indicano lo stesso
entry; questo fallthrough non deve puntare a `JUMPDEST`. Il nonzero/true edge è
il mismatch ed esclude il selector; `ISZERO` trasforma la stessa espressione in
uguaglianza. Selector word, calldata word zero, calldata size e call value guard
sono raffinati edge per edge. Una condizione unknown interrompe la prova invece
di seguire un branch solo possibile.

Dopo il riconoscimento di un candidato funzione, il traversal del suo scope
prosegue con l’`exact singleton selector` del candidato. Se la funzione torna al
dispatcher condiviso, `SelectorEquality`, `XOR` grezzo e `SelectorWord` seguono
soltanto il `definite edge` coerente con il selector già abbinato. I predicati
Unknown o non correlati mantengono prudentemente tutti i `definite edges`. Non
si usa l’euristica che esclude gli altri entry block: il flusso legittimo
`shared body/tail-call` resta nello scope della funzione.

Gli esiti esterni di CALL/CREATE sono distinti: l’esito host è davvero non
deterministico, quindi l’analisi esplora entrambi gli edge CFG precisi. Mantiene
così la recovery del fallback ERC-1167 senza trattare una condizione selector
illeggibile come prova; un dispatcher realmente Unknown continua a fallire chiuso.

`EVMAnalysisLimits.def` assegna a decoder lineare e CFG builder un unico budget
aggregato di diagnostic LowIR tramite `MaxLowDiagnostics` e
`MaxLowDiagnosticBytes`. Entrambi i percorsi preaddebitano count esatto e byte
finali e rifiutano un limite zero. I budget diagnostic LowIR e HighIR restano
indipendenti. La stessa tabella addebita `MaxHighDispatchCandidates`,
l’aggregato globale `MaxHighRecoveredArguments`, `MaxHighDiagnostics` con
`MaxHighDiagnosticBytes`, `MaxHighReferenceVisits`,
`MaxHighMemoryTransferCells` e `MaxHighMemoryValueVisits`. I record di candidato
e argomento recuperato sono preaddebitati prima di inserirli in qualunque
container di destinazione o allocare nome/tipo. Ogni diagnostic di output
HighIR addebita count e byte finali del messaggio prima di essere costruito o
copiato, incluso il diagnostic fisso per IR malformed; esaurire il budget
restituisce il relativo hard error nominato senza omettere silenziosamente
diagnostic o fact.
La root CFG region predefinita addebita `MaxHighRegionBlockReferences` prima di
reserve o copia della lista di block PC.

`EVMABIParserLimits.def` limita nesting delle tuple, nodi di tipo e dimensioni
array aggregate. `EVMABITableLimits.def` limita cardinalità e testo aggregato
delle tabelle pubbliche di signature/variant. La validazione pubblica applica i
limiti prima di parse o hash, poi rifiuta enum invalidi, metadata di kind,
standard, ruoli di selector evidence, tipi non canonici, hash derivati,
membership e collisioni. Il lookup selector di produzione è indicizzato, il
lookup event usa una tabella ordinata per topic e le API topic verificano che un
`APInt` sia largo esattamente una word EVM prima di confronto o ordinamento.

`EVMInterpreterLimits.def` dichiara `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, l’aggregato `MaxLogDataBytes`, l’aggregato
`MaxHostReturnDataBytes`, `MaxCalldataBytes`, l’aggregato
`MaxHostEnvironmentEntries`, l’aggregato `MaxExternalCodeBytes` e
`MaxPersistentStateEntries`. L’aggregato degli host entry include `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode` e `BlobHashes`; il limite byte include
tutti i body `ExternalCode`. `MaxSteps` conserva il
risultato esplicito `StepLimit`. La crescita runtime di memory, trace, log, dati
log e nuove chiavi di stato persistente è preaddebitata; superare questi limiti
restituisce `ResourceExhausted` e ripristina stato persistente, log ed effetti
selfdestruct. Un aggregato iniziale di host return data o una map di stato
persistente troppo grande è invece un errore API di `execute`. L’interpreter
mantiene host return data come view `ArrayRef` e usa `lower_bound` sulla tabella
di istruzioni ordinata e già validata, senza copiare buffer né ricostruire una
PC map per ogni esecuzione. Il `const execute preflight` valida programma e
limiti host prima di copiare environment, snapshot o result.

### Audit differenziale live di go-ethereum

L’audit locale standard e CI forzano a ogni esecuzione
`git fetch --depth=1 --force` del `HEAD` remoto del branch predefinito del repository
ufficiale `https://github.com/ethereum/go-ethereum.git`. Ogni esecuzione crea
un repository bare privato, temporaneo e dal nome
imprevedibile; non esistono repository Git persistenti o cache condivisi.
Soltanto l’authority ref restituito dal fetch e lo SHA esatto risolto da esso
scelgono la revisione. Lo SHA viene riportato e provato in un worktree
temporaneo detached; poi authority repository e worktree vengono distrutti
insieme. Né `local_docs`, né un checkout esistente, né un
submodule sono percorsi d’audit; un pin di submodule diventerebbe obsoleto
proprio quando serve rilevare il drift live.

Ogni comando Git rimuove prima tutti i `GIT_*` ereditati, inclusi
`GIT_CONFIG_*`, e poi installa soltanto valori revisionati.
`GIT_CONFIG_NOSYSTEM` e `GIT_CONFIG_GLOBAL` disabilitano la configurazione
system/global; `GIT_ATTR_NOSYSTEM` e `core.attributesFile` per comando
disabilitano gli attributi system/global, mentre `core.hooksPath` disabilita gli
hook. Il repository privato rifiuta configurazione locale inattesa,
grafts, `objects/info/alternates` e `refs/replace`;
`GIT_NO_REPLACE_OBJECTS` disabilita inoltre le sostituzioni. Ogni deviazione
fallisce in modo chiuso.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

La CLI pubblica espone soltanto `--manifest-output`; source, ref e toolchain non
sono selezionabili. Il suo manifest chiuso usa `schema 3`. Il probe Go riflette
l’intero inventario booleano esportato da `params.Rules`,
chiama `LookupInstructionSet(params.Rules)` per ogni fork mappato e scandisce tutti
i 256 slot di byte. L’allocazione è decisa soltanto da `operation.undefined` di
geth; `HasCost` è solo un controllo incrociato del costo, perché è false anche
per operazioni definite a costo zero. Ogni slot `defined && !HasCost` deve
corrispondere esattamente a `EVM_GETH_ACTIVE_WITHOUT_COST` dal fork di attivazione
dichiarato; uno slot undefined con costo, uno definito non revisionato o la
scomparsa upstream del marker falliscono in modo chiuso. Field e record
sconosciuti, duplicati, mancanti, fuori range o non analizzati sono errori. Ogni
`.def parser` rifiuta inoltre input simile a macro ma non consumato, invece di
accettare una policy `partial`.
`EVMUpstreamOpcodePolicy.def` gestisce alias e typed exclusions storiche/EOF non
pianificate e ne valida gli invarianti overlap/inactive. L’ortogonale
`EVMUpstreamSemanticsPolicy.def` gestisce l’inventario chiuso e riflesso di
`params.Rules`, la mappatura dei fork, le eccezioni base-stack e le famiglie
dynamic-immediate. CI gira sui push a `dev`, sulle pull request, manualmente
e ogni giorno; in caso di errore carica revisione esatta, manifest e log come
artifact.

In particolare, `EVMUpstreamSemanticsPolicy.def` assegna ogni campo booleano
esportato di `params.Rules` a una sola entry `EVM_GETH_RULE_FIELD` di categoria
`MappedForkSelector`, `NoOpcodeAllocation` o
`ExcludedSelectorExpectedError`. L’audit abilita ogni campo isolatamente e
chiama `LookupInstructionSet`: le prime due categorie richiedono nil error, la
terza error; il fingerprint opcode/stack completo di 256 slot deve sempre
eguagliare `ExpectedFork`. I campi senza allocazione `IsEIP155`, `IsEIP2929`,
`IsEIP4762` e `IsPetersburg` producono Frontier; `IsUBT` deve fallire e produrre
il fingerprint Cancun.

`EVMUpstreamSemanticsPolicy.def` dichiara gli opcode di ogni famiglia dinamica
EIP-8024, il tipo di operazione e il delta stack valido;
`EVMEIP8024Immediates.def` resta l’autorità distinta per il decode degli
immediate e classifica tutti i valori single/pair. Con `go -overlay`, l’audit
ottiene i veri handler privati `operation.execute` e copre una per una le
`canonical fork jump tables` e le `mainnet active/scheduled jump tables`. Una
famiglia `inactive` è registrata esplicitamente; una famiglia `partial` è un
errore. Per ogni tabella attiva esegue le tre operazioni dichiarate su tutti gli
immediate (`3x256`) più i `3 missing-operand cases`, verificando accettazione,
delta PC, operandi/mutazione derivati dai marker, underflow esatto e comportamento
`0x00` mancante rispetto alle stesse policy dichiarative.

`EVM_HARDFORK_LATEST` ha esattamente un target canonico. La mappa chiusa
`EVMUpstreamForkAliases.def` porta Prague a Pectra, Osaka e BPO1 fino a BPO5 a
Fusaka; Paris, Shanghai, Cancun, Amsterdam e Bogota sono identity. Un nome nuovo
sconosciuto fallisce chiuso. Ogni audit fissa e registra un `audit_unix_time`,
richiede che `MainnetChainConfig.LatestFork(time)` corrisponda al latest di NeverD
e che `LatestFork(max uint64)` sia nell’inventory degli alias con il fork
canonico già provato; entrambe le instruction table sono confrontate per intero.
Il manifest registra `authority=official-fresh-fetch`, URL ufficiale, `HEAD`
richiesto e SHA risolto. Il probe fissa `GOTOOLCHAIN=local`.

Go e Python applicano `input/collection/string hard limits` prima di
materializzare metadata ostili; input, collection o stringhe sovradimensionate
falliscono in modo chiuso. Per `bounded diagnostic output`, una visualizzazione
troppo lunga porta il `digest` del contenuto completo e un
`explicit truncated marker`. Ogni child process ha output e deadline limitati;
un superamento termina l’intero `process group`/process tree e ne drena le pipe.

La ricevuta live schema 3 corrente registra `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revisione
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, `Go 1.24.0` locale,
`stack_limit=1024` e `diagnostics=[]`. Confronta `21 fork tables` e
`20 Rules probes`, classificati `15 mapped/4 no-op/1 expected-error`. Entrambi i
record `mainnet active/scheduled` indicano `upstream BPO2`, che l’alias chiuso
mappa a `NeverD Fusaka`. EIP-8024 copre `23 table targets`; soltanto
`Amsterdam/Bogota` sono attivi, con `1536 candidate executions` e
`6 missing-operand cases`. I `three handler symbols` coincidono sui due target
attivi. Audit Python `67/67` e `C++ Opcode 10/10` sono verdi. Su macOS l’audit
reale è riuscito sotto `sandbox-exec`, con il `go run` finale offline; il workflow
Linux impone `bubblewrap`.

Tutte le fasi Go — `go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download` e `go run` — si eseguono in un filesystem sandbox
`capability-root`. Sono leggibili soltanto probe privato, geth fresco,
`resolved GOROOT` validato e le precise root di runtime di sistema necessarie;
sono scrivibili solo le root isolate dell’ambiente. La rete è concessa soltanto
alle fasi dependency che la richiedono e il run finale resta offline. I
sentinels nel `host HOME/workspace` sono negati e il loro contenuto non può
apparire nell’output. Linux replica la policy con `bubblewrap` senza `/` broad bind.

`NeverDEVMDecoderPropertyTests` esaurisce tutti gli input di due byte per ogni
fork che cambia il decoder, confronta il decode completo e i confini `JUMPDEST`
esatti, quindi invia a tutti i fork stringhe di byte ostili deterministiche di
lunghezza limitata.

Le lane LowIR dell’intero stack preservano le correlazioni. `MayReachable` è solo
un candidato CFG. Uno slot loop-carried modificato diventa semanticamente `Top`
al back-edge, indipendentemente dai budget; l’esaurimento fallisce senza
`emergency widening`. La memoria HighIR segue write costanti, overlap/kill e invalidazione
unknown. Sono provati selector e byte Panic noti; una dichiarazione custom error
nota conserva i tipi canonici, senza affermare ogni valore runtime. Gli altri
fatti restano candidati supportati da evidenze.

## Contratto C generato

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Le operation ambientali usano la seguente host ABI. `a0` è il top originale,
gli argomenti inutilizzati sono zero e il ritorno è il primo valore pushed. Il
trace hook viene eseguito prima di ogni istruzione.

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment, uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);
void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Il frontend deve supportare `_BitInt` almeno a 512 bit. Apple Clang Darwin non
lo fa ancora; su macOS usa un target non-Darwin adatto o direttamente LLVM.

## Contratto Solidity generato

L’output combina dichiarazioni function/storage/event/error per selector con
una PC/stack state machine esatta. Uno slot costante viene emesso come
`recovered_storage_slot_3 = uint256(0x3)`, mai come variabile sequenziale che
inventi il layout.

Il contract è volutamente `abstract`. Override `_evmHost` per gli effetti
ambientali; `_evmTrace` è virtual ed emette `EVMTrace` per default.

```bash
solc --bin contract.sol
```

## C API

```c
neverd_session_t session = neverd_session_create();
neverd_evm_set_hardfork(session, "cancun");
neverd_evm_set_strict(session, 1);
if (!neverd_session_load(session, "contract.evm") ||
    !neverd_session_analyze(session)) {
  /* inspect neverd_last_error(session) */
}
const char *solidity = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_SOLIDITY, 0, 0);
const char *c = neverd_decompile_all_ex(
    session, "contract.evm", NEVERD_OUTPUT_C, 0, 0);
neverd_free_string(solidity);
neverd_free_string(c);
neverd_session_destroy(session);
```

`neverd_decompile_all` resta compatibile ed emette C. Le nuove entry sono
`neverd_session_bitness`, `neverd_evm_set_strict`,
`neverd_evm_set_hardfork` e `neverd_decompile_all_ex`. Solidity per native,
legacy LLVM-to-C per EVM e native-object roundtrip per EVM sono rifiutati
esplicitamente, mai ignorati.

## Limiti espliciti

- Solo legacy bytecode; i container EOF non vengono ancora decodificati.
- Amsterdam/Bogota sono target di sviluppo espliciti; `latest` resta Fusaka
  finalizzato finché gli opcode pianificati non vengono finalizzati.
- Nessun RPC, chain-state discovery, gas/refund o esecuzione precompile.
- Creation extraction riconosce wrapper statici, non una transazione completa.
- I dynamic jump restano indiretti salvo prova costante limitata.
- Tipi ABI, nomi, mapping, event e custom error sono recovery best-effort.
- L’esecuzione autonoma degli effetti richiede host hook C/Solidity.
