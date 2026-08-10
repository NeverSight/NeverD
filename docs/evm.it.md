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
limitati ed estrae la slice runtime copiata. Un field con il solo `0x` opzionale
è vuoto, così un runtime vuoto non nasconde un fallback di creazione utile. La
mappa CBOR Solidity finale viene rimossa solo se lunghezza, marker e una chiave
`solc`, `ipfs` o Swarm nota risultano validi.

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
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def` e
  `OutputLanguages.def` generano enum ordinate, parser, nomi, scelte CLI e
  valori C ABI. `EVMConstants.h` centralizza width, limiti e nomi.
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

- **LowIR** conserva PC, encoding, immediate PUSH con right-zero padding,
  block, edge, target `JUMPDEST` validati, reachability e stack height.
- **MedIR** rappresenta lo stack come SSA 256-bit, crea phi, fa constant folding
  delle pure operation e mantiene effect, memory, state e call-value ortogonali.
- **HighIR** recupera selector, parole calldata/return probabili, mutability,
  slot costanti, event, revert e regioni function/CFG best-effort. Payability e
  state lattice sono indipendenti. Un dynamic jump raggiungibile non risolto
  unisce a `Unknown` e rende Solidity `nonpayable`; selector in conflitto sono
  diagnosticati e omessi.
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
