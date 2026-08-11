**Lingue**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# Decompilazione EVM

[← Indice della documentazione](README.it.md)

NeverD carica il bytecode tradizionale dell’Ethereum Virtual Machine, costruisce
LowIR dedicato a 256 bit, MedIR stack-SSA e HighIR recuperato, quindi emette LLVM
IR, C23 o Solidity. L’analisi strict è predefinita: un opcode non assegnato o
inattivo per l’hardfork scelto produce un errore al PC esatto.

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
spostare il confine. Un field con il solo prefisso `0x` opzionale è vuoto, così
un `deployedBytecode` o `runtimeBytecode` vuoto non nasconde un fallback di
creazione utile.

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
multi-contract ambigui, limiti metadata invalidi o code vuoto producono errori
utilizzabili. `BytecodeLoadOptions::ArtifactContract` seleziona `Contract` o
`path/File.sol:Contract`. Se più source file definiscono lo stesso nome, quello
non qualificato è rifiutato per evitare selezioni dipendenti dall’ordine JSON.

EVM è registrato nel core loader registry, non nascosto in un backend plugin.
CLI, C API, disassembler, CFG e query IR usano quindi la stessa image normalizzata
e le stesse opzioni.

## Hardfork e opcode

Sono coperti tutti i 150 opcode legacy assegnati da Frontier a Fusaka, inclusi
`PUSH0`, transient storage, `MCOPY`, opcode blob e `CLZ`. `latest` seleziona
Fusaka per impostazione predefinita.

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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2) ed è
segnato come [rimosso da Osaka e non pianificato](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD non tratta la proposta ritirata come comportamento mainnet.

Strict rifiuta byte ignoti o inattivi. `--evm-relaxed` li conserva in LowIR e
nei diagnostics, ma i backend generano fault se vengono eseguiti; non diventano
mai silenziosamente NOP.

## Architettura metadata in stile LLVM

I metadata EVM mantenuti a mano seguono il pattern `.def` multi-incluso di LLVM:

- `EVMOpcodes.def` è l’unica fonte per 150 opcode finalizzati e quattro opcode
  di sviluppo opt-in. Encoding, mutazioni pop/push reali, tipo di immediate,
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
  esclusioni storiche/ritirate intenzionali;
  `scripts/audit_evm_opcode_metadata.py` rifiuta byte drift e ogni nuova
  costante upstream non revisionata.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` e
  `OutputLanguages.def` generano enum ordinate, parser, nomi, scelte CLI e
  valori C ABI. `EVMConstants.h` centralizza width, limiti e nomi.
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
  `EVMKnownSignatures.def` fa lo stesso per i tre ruoli di una firma.
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

  `AnalyzeOptions::MaxAbstractValuesPerSlot` limita ogni insieme finito; il
  superamento allarga lo slot a `Unknown`. `MaxStackHeightVariants` limita il
  numero di altezze dipendenti dal percorso in un block e produce un errore
  esplicito di limite d’analisi invece di troncare il CFG. Entrambi i limiti
  rifiutano zero. I valori finiti creati da un’operazione cartesiana dopo un
  merge dello stack non relazionale sono marcati come over-approximation: i
  candidati non validi sono diagnosticati, ma non possono far rifiutare il
  bytecode dall’analisi strict solo perché è stata persa la correlazione fra
  slot. I target precisamente non validi falliscono ancora all’esatto jump PC.
  In modalità relaxed, gli stack fault sono diagnosticati e terminano soltanto
  il percorso astratto errato; non viene inventato alcun fallthrough impossibile.
- **EVM MedIR** rappresenta ogni valore dello stack come SSA a 256 bit e collega
  tutti i merge phi prima di eseguire una sparse constant worklist deterministica.
  Il lattice privato è `Uninitialized`, una `Constant` esatta oppure
  `Overdefined`: costanti uguali si propagano tra block e cicli phi ancorati,
  mentre un ciclo in conflitto o dipendente dal runtime non può inventare una
  costante. La worklist controlla gli ID def-use e usa lo stesso valutatore ALU
  di `Semantics.h` dell’interpreter. MedIR conserva anche l’effect semantico
  primario più, ortogonalmente, l’accesso EVM-memory
  `none/read/write/readwrite`, lo state access a livello sorgente e il call-value
  access. A questo confine uno stack LowIR polimorfo viene allineato
  conservativamente al top; gli slot assenti su alcuni percorsi diventano
  valori unknown espliciti e un diagnostic deterministico registra la perdita
  di precisione.
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
  risolto impone un recupero `nonpayable` conservativo. Finché MedIR non avrà
  memory SSA, il recupero del payload di custom error e quello della chiamata
  uscente resteranno le uniche euristiche con instruction window limitata; nomi e tipi recuperati restano esplicitamente
  euristici.

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
