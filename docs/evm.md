**Languages**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM decompilation

[← Documentation index](README.md)

NeverD loads legacy Ethereum Virtual Machine bytecode, builds dedicated
256-bit LowIR, stack-SSA MedIR, and recovered HighIR representations, and can
emit LLVM IR, C23, or Solidity. Strict analysis is the default: an unassigned
opcode or an opcode that is inactive for the selected hardfork is an error at
its exact program counter.

The Solidity and C outputs are semantic reconstructions. They preserve decoded
opcode order, 256-bit arithmetic, stack checks, and validated control flow, but
they do not claim to reproduce the contract's original source, identifiers, or
types.

## Quick start

```bash
# Lift to verified LLVM IR using i256/i512 values.
./build/bin/neverd lift contract.evm -o contract.ll

# Inspect each EVM analysis stage.
./build/bin/neverd lift --dump-low contract.evm
./build/bin/neverd lift --dump-med contract.evm
./build/bin/neverd lift --dump-high contract.evm

# Emit either supported source language.
./build/bin/neverd decompile --language=c contract.evm -o contract.c
./build/bin/neverd decompile --language=solidity contract.evm -o contract.sol

# Select historical opcode availability or retain unknown opcodes as explicit
# fault nodes for forensic analysis.
./build/bin/neverd decompile --language=solidity \
  --evm-hardfork=cancun --evm-relaxed contract.evm
```

`disasm`, `cfg`, and the C API's Low/Med/High/LLVM queries also accept EVM
inputs. EVM binary rewriting is deliberately rejected; `patch` remains a
native-binary operation.

## Accepted inputs

| Input | Recognition and normalization |
|-------|-------------------------------|
| Raw bytes | `.raw` or `.evmraw`, or binary content in an explicit EVM extension |
| Hex text | Optional `0x`, arbitrary ASCII whitespace, extensions `.evm`, `.hex`, `.bin`, or `.bytecode`; validated extension-free hex is also detected |
| Compiler artifact | `.json` with root or `evm`-nested `deployedBytecode`, `runtimeBytecode`, or `bytecode`; solc standard JSON under `contracts → file → contract → evm` is supported |

Runtime/deployed bytecode is preferred over creation bytecode. When only
creation code is available, NeverD recognizes bounded, constant
`CODECOPY`/`RETURN` constructor wrappers and extracts the copied runtime slice.
The constructor walk uses the same single-instruction decoder as the real
decoder, under the hardfork being analyzed, so a byte that is data on one fork
and an opcode on another cannot move the boundary. A compiler artifact field
containing only an optional `0x` prefix is treated as empty, so an empty
`deployedBytecode` or `runtimeBytecode` field cannot hide a usable
creation-bytecode fallback.

### Compiler trailers

`EVMMetadataFields.def` tabulates both trailer formats. Solidity writes a CBOR
map whose two trailing bytes count the map alone; `vyper` writes a CBOR array
ending in that map, whose two trailing bytes count the whole footer including
themselves. Reading one framing as the other does not fail loudly — it lands
two bytes away and removes two bytes of real code — so both are attempted and an
input matching neither is left alone.

The trailer is read twice: once on the input as given and once on the runtime
code that remains after a deployment wrapper is unwrapped. Vyper moved its
trailer into the initcode and leaves the runtime code without one, so a reader
that only looks after unwrapping reports an unknown build for a contract that
named itself. A sequence footer also states the runtime code length, the data
section lengths and the immutables length, which bound the returned code
without executing the constructor.

### Containers that are not instructions

`EVMBytecodeContainers.def` classifies the input before any decoding. Since
EIP-3541 made `0xEF` undeployable, a leading `0xEF` promises the bytes are not
instructions:

| Container | Marker | Disposition |
|-----------|--------|-------------|
| legacy | — | decoded as instructions |
| delegation (`eip-7702`) | `0xef0100` and exactly 23 bytes | reports the target account; analysis stops |
| eof (`eip-3540`) | `0xef00` | rejected; no fork has activated it |

A delegation indicator's twenty bytes are an address, not code. Decoding them
would read the address as opcodes and produce a control-flow graph of an
account, so `info` reports the target and analysis refuses with the reason. The
refusal distinguishes the two cases: before Pectra the marker is not assigned
yet, and from Pectra on the target's runtime code is simply missing. A marker at
any other length is malformed input rather than a variant of the container, and
stays instructions so the decoder can name the byte it could not read.

Malformed hex, odd digit counts, unresolved linker placeholders, ambiguous
multi-contract artifacts, invalid metadata bounds, and empty normalized code
produce actionable errors. The C++ loader API can select a contract from a
multi-contract artifact with `BytecodeLoadOptions::ArtifactContract`, using
either `Contract` or `path/File.sol:Contract`. An unqualified contract name is
rejected when multiple source files define it; use the qualified spelling so
artifact ordering can never silently select the wrong bytecode.

EVM is registered in NeverD's core loader registry rather than hidden behind a
backend plugin. Consequently, the CLI, C API, disassembler, CFG builder, and
Low/Med/High/LLVM query paths all receive the same normalized image and EVM
options; format recognition and semantic analysis cannot drift between entry
points.

## Hardforks and opcodes

The finalized legacy opcode set is covered from Frontier through Fusaka,
including `PUSH0`, transient storage, `MCOPY`, blob opcodes, and `CLZ`.
Amsterdam's four scheduled opcodes are also implemented behind an explicit
development-fork target. The default `latest` target remains Fusaka. Accepted
names are:

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, amsterdam, bogota, latest
```

Common aliases are accepted: `dao`, underscore spellings such as
`tangerine_whistle`, `merge`, `prague`, `osaka`, and `glamsterdam`. `latest`
and `osaka` currently resolve to the canonical `fusaka` execution revision;
`glamsterdam` resolves to `amsterdam`.

`latest` deliberately means the latest finalized mainnet revision implemented
by NeverD, not the tip of Ethereum's development branch. Ethereum currently
describes [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) as an
upcoming Q4 2026 upgrade. NeverD exposes its scheduled-but-still-Review-stage
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843),
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) instructions
only when `--evm-hardfork=amsterdam` (or `bogota`) is selected. They remain
outside `latest` until the fork and encodings are finalized. This matters
especially for EIP-8024: a valid immediate is consumed, an invalid candidate
remains the next instruction byte, and a missing byte has semantic value zero.
Treating it as an ordinary fixed-width immediate would corrupt instruction and
`JUMPDEST` boundaries.

EOF is not part of the Fusaka target: Ethereum removed it from the upgrade in
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2), and
the execution-spec-tests project records EOF as
[removed from Osaka and unscheduled](https://github.com/ethereum/execution-spec-tests/blob/main/docs/CHANGELOG.md).
NeverD therefore does not accept the withdrawn EOF proposal as if it were
finalized mainnet behavior.

Strict mode rejects unknown and fork-inactive bytes. `--evm-relaxed` retains
them in LowIR and diagnostics, but generated backends still fault if execution
reaches one; relaxed mode never silently treats an unknown byte as a NOP.

## LLVM-style metadata architecture

Hand-maintained EVM metadata follows LLVM's multiply-included `.def` pattern:

- `EVMOpcodes.def` is the single source of truth for 150 finalized and four
  opt-in development-fork opcodes: encoding, actual pop/push mutations,
  immediate encoding, opcode class, activation fork, primary effect,
  orthogonal EVM-memory access,
  source-level state access, call-value access, and termination. Every required
  property is on the same record, so adding an opcode cannot silently inherit
  a default.
- `EVMMemoryAccesses.def`, `EVMStateAccesses.def`, and
  `EVMCallValueAccesses.def` define the closed, named
  property domains. The per-opcode values stay typed and orthogonal:
  `CALL` is both an external call and a memory read/write, while
  `EXTCODECOPY` is both a context read and a memory write. State access uses
  the explicit `None/Read/Write/Unknown` lattice rather than two booleans with
  an invalid combination. Payability remains an orthogonal source-level
  constraint: `CALLVALUE` carries a typed call-value-read property alongside
  its general context-read effect, so a
  recovered region that reads `msg.value` is emitted `payable` rather than the
  unsound `view` classification. The analyzer separately recognizes the
  canonical `ISZERO(CALLVALUE)` non-payable guard, verifies that its non-zero
  branch ends in `REVERT`, and suppresses only that compiler-generated read.
- `EVMImmediateKinds.def` defines fixed PUSH data and EIP-8024's conditional
  single/pair encodings; `EVMDecodeStatuses.def` owns the stable vocabulary
  exposed by LowIR and disassembly. `EVMUpstreamOpcodePolicy.def` records the
  sole go-ethereum naming alias plus deliberate historical/withdrawn
  exclusions; `scripts/audit_evm_opcode_metadata.py` rejects byte drift and
  every new unreviewed upstream constant.
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def`, and
  `OutputLanguages.def` provide the corresponding ordered enums, parsers,
  display names, CLI choices, and C ABI values.
- `EVMCalls.def` describes the four instructions that call another program and
  the lattice of places a callee address can come from. One flag per record,
  whether a value operand sits between the callee and the argument window,
  derives every later operand position, and the table is validated against the
  opcode database so the derivation cannot drift from the declared pop counts.
- `EVMPrecompiles.def` is the dictionary of addresses the protocol answers at
  itself, each with the fork that reserved it and the proposal that scheduled
  it. `P256VERIFY` at `0x100` is credited to `eip-7951`, which is the Final
  proposal that reserved it on mainnet with Fusaka; the rollup proposal its
  interface came from never scheduled it. Gas is deliberately absent: a
  precompile's cost is a function of its input and has been repriced without
  the address or the operation changing.
- `EVMMetadataFields.def` and `EVMBytecodeContainers.def` describe what an
  input is before it is decoded: the two compiler trailer framings, and the
  containers whose bytes are not instructions at all.
- `EVMRecoveredFacts.def` owns the spellings of the recovered-fact
  vocabularies, so a name that reaches output lives in one place rather than
  in a switch a new enumerator can be left out of. `EVMKnownSignatures.def`
  does the same for the three roles a signature holds.
- `EVMConstants.h` owns protocol widths, limits, and stable default names.
- `Semantics.h` owns the target-independent scalar ALU evaluator. Constant
  folding and interpretation call the same checked `APInt` implementation;
  LLVM, C, and Solidity retain explicit target lowerings so backend contracts
  and unsupported cases remain visible.

The dedicated decoder is the raw-byte boundary; CFG and stack analysis consume
its lossless result in a later stage. Assigned identity, fork activation,
immediate validity, actual pop/push mutations, and required pre-execution stack
height are deliberately separate. Relaxed decoding preserves an assigned
instruction's identity without giving an inactive opcode semantics or letting
its would-be immediate shift later boundaries. EIP-8024 depth operands are
decoded once into typed instruction fields, and every analyzer/interpreter/
backend consumes those fields rather than re-decoding bytes. Raw encodings
reappear only at byte-oriented ABI boundaries such as tracing and host
callbacks. The largest dynamic stack requirement is 236 words, while the
largest non-stack host operation has seven arguments; both limits are named
and independently derived or validated.

`OpcodeInfo` cannot be default-constructed into a half-valid record, and its
name is an `llvm::StringLiteral`, not a potentially dangling `StringRef`. A
compile-time table validator rejects duplicate encodings, unknown classes or
properties on assigned rows, invalid scalar ALU contracts,
effect/state-access mismatches, incorrect immediate/stack-family contracts,
and branch opcodes not marked as basic-block terminators. It also rejects a
non-stack opcode with more than one pushed result because the shared host ABI
returns one word, and rejects an unrecognized stack-family opcode until its
dedicated lowering exists. Relaxed decoding obtains conservative faulting
metadata only through the explicit unknown-byte factory.

The `.def` files are intentionally hand-authored databases, analogous to
LLVM's
[`Instruction.def`](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/IR/Instruction.def).
Within the EVM subsystem, `.inc` is reserved for an actual generated or literal
include fragment (for example, TableGen output), so hand-maintained record
databases are not disguised as generated output. Target-language templates
remain named and local to their C or Solidity emitter instead of being forced
into metadata. The surrounding C++ follows LLVM's
[coding standards](https://llvm.org/docs/CodingStandards.html), including LLVM
ADT/string types at public boundaries and fail-loud exhaustive semantic
switches.

That split follows LLVM's own model: small hand-maintained X-macro databases
use `.def`, while richer declarative records live in `.td` inputs and
[TableGen](https://llvm.org/docs/TableGen/ProgRef.html) emits the `.inc` files
consumed by C++. NeverD does not currently own a TableGen generation step, so a
checked-in generated-looking EVM `.inc` would add ceremony without a source
generator.

To add an opcode, add one complete `EVM_OPCODE` record, then add its shared
scalar semantics where applicable, explicit backend lowering cases, and
focused tests. To add a hardfork, add one ordered `EVM_HARDFORK` record plus
any aliases. The typed API, lookup tables, validation, classification, and CLI
values expand without parallel numeric or string tables; backend-specific
semantic switches remain explicit and fail loudly if an ALU case is omitted.

## Analysis model

- **EVM LowIR** preserves PC, encoding, typed immediate status and decoded
  stack-depth operands (including PUSH right-zero padding and EIP-8024's
  conditional-consumption rule), basic blocks, predecessor/successor edges,
  validated `JUMPDEST` targets, reachability, and stack-height domains. CFG
  recovery is a deterministic whole-program fixed point: one bounded finite
  set of 256-bit values is propagated per stack slot and one abstract stack is
  retained per concrete height. Constants carried through internal-call and
  return blocks, stack shuffles, `PC`/`CODESIZE`, and scalar ALU operations can
  therefore resolve one or several concrete jump targets. A genuinely unknown
  target remains an explicit indirect edge instead of being guessed.

  `AnalyzeOptions::MaxAbstractValuesPerSlot` bounds each finite value set;
  exceeding it widens the slot to `Unknown`. `MaxStackHeightVariants` bounds
  the number of path-dependent heights at a block and produces an explicit
  analysis-limit error instead of truncating the CFG. Both limits reject zero.
  Finite values created by a Cartesian operation after a non-relational stack
  merge are marked as over-approximations: invalid candidates are diagnosed,
  but cannot make strict analysis reject bytecode solely because slot
  correlation was lost. Precise invalid targets still fail at the exact jump
  PC. In relaxed mode, stack faults are diagnosed and terminate only the
  faulting abstract path; no impossible post-fault fallthrough is fabricated.
- **EVM MedIR** represents every stack value as a 256-bit SSA value and wires
  all merge phis before running a deterministic sparse constant worklist. Its
  private lattice is `Uninitialized`, one exact `Constant`, or `Overdefined`:
  equal constants propagate across blocks and anchored phi cycles, while a
  conflicting or runtime-dependent cycle cannot fabricate a constant. The
  worklist uses checked def-use IDs and the same `Semantics.h` ALU evaluator as
  the interpreter. MedIR also preserves the primary semantic effect plus
  orthogonal `none/read/write/readwrite` EVM-memory, source-level state, and
  call-value access. A polymorphic LowIR stack is conservatively top-aligned at
  this boundary: slots absent on some incoming height become explicit unknown
  values and a deterministic diagnostic records the precision loss.
- **EVM HighIR** recovers Solidity dispatcher selectors, likely calldata and
  return words, mutability, constant storage slots, LOG/event facts, revert
  facts, and function/CFG regions. A checked producer index and iterative,
  memoized value walk recover facts from typed MedIR operands, not instruction
  distance: selector comparisons may cross blocks and phis, use either `EQ`
  operand order, and retain a derived 32-bit mask; argument offsets, storage
  keys, event topic0, non-payable and receive guards, and exact 32-byte return
  sizes use their semantic inputs. The iterative walk is structurally bounded
  by the MedIR graph and treats malformed, mixed, or cyclic expressions as
  unknown. Conflicting dispatcher targets for the same selector are diagnosed
  and omitted. Solidity payability remains independent from the state-access
  lattice, and a reachable unresolved dynamic jump forces conservative
  `nonpayable` recovery. Until MedIR gains memory SSA, custom-error and
  outgoing-call payload recovery are the retained bounded instruction-window
  heuristics; recovered names and types remain explicitly heuristic.

  HighIR also records the outgoing half of the interface: every `CALL`,
  `CALLCODE`, `DELEGATECALL`, and `STATICCALL`, with the callee's provenance,
  the reserved address it names when the analyzed fork reserves one, the
  selector the call places at the head of the callee's calldata, and the
  transferred value when it is constant. `CREATE` and `CREATE2` are excluded
  because they run code that has no address yet, so there is no callee to
  recover.

  A recovered outgoing signature never joins the standards the program answers
  to. Sending `transfer(address,uint256)` says the program uses a token, not
  that it is one, and conflating the two would report every router and vault
  as an ERC-20. A delegating call is additionally reported as a proxy fact,
  because it is the only member of the family whose callee runs against this
  program's own storage.

  The precompile lookup is gated on the fork being analyzed rather than on the
  newest one that exists. Calling the address of a precompile a later fork
  introduces reaches an account with no code, which succeeds and returns
  nothing, so naming it would report an operation the program provably did not
  perform.
- **LLVM** emits a verifier-clean `i32 @evm_execute(ptr)` state machine with a
  checked 1024-word `i256` stack, `i512` modular intermediates, guarded signed
  division, saturated shifts, exact `BYTE`/`SIGNEXTEND`/`CLZ`, and validated
  dynamic-jump switches.

The built-in deterministic interpreter is the semantic oracle used by the
test suite. LLVM and generated C are compiled and executed against it; emitted
Solidity is compiled, deployed to Anvil, called, and compared for observable
storage and trace behavior. A pre-Fusaka corpus is also installed as raw
bytecode into Anvil and executed by its native EVM. It covers the scalar ALU,
calldata copying, overlapping `MCOPY`, memory expansion, Keccak, and return
data, providing an independent interpreter-to-client comparison rather than
relying only on backends that could share the same lowering mistake.
Account-taking opcodes follow the
[execution specification](https://github.com/ethereum/execution-specs/blob/master/src/ethereum/forks/osaka/vm/instructions/environment.py)
and mask stack operands to the protocol's 160-bit address width, while public
environment values and maps are validated before execution so malformed
`APInt` widths cannot reach LLVM assertions. `BLOCKHASH` also enforces the
protocol's previous-256-block window rather than trusting an out-of-range mock
environment entry.

The interpreter also keeps EIP-211's per-frame return-data buffer separate
from the current frame's final output. `RETURNDATASIZE` and
`RETURNDATACOPY` observe the latest subcall buffer; only `RETURN` or `REVERT`
populate `ExecutionResult::ReturnData`. Consequently, `STOP` after a call
cannot accidentally expose the callee's bytes as the caller's return value.
The deterministic CREATE/CREATE2 model follows the same rule: failed creation
pushes zero and exposes configured revert bytes through the EIP-211 buffer,
while successful creation pushes the configured address and clears that
buffer. `InitialReturnData` is an explicit snapshot/testing seed, not the
current frame's final output.

## Generated C contract

C output uses the C23 extended integer types below so arithmetic is not
truncated to 64 or 128 bits:

```c
#define NEVERD_EVM_WORD_BITS 256u
#define NEVERD_EVM_WIDE_WORD_BITS (2u * NEVERD_EVM_WORD_BITS)
typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;
typedef signed _BitInt(NEVERD_EVM_WORD_BITS) evm_sword;
typedef unsigned _BitInt(NEVERD_EVM_WIDE_WORD_BITS) evm_wide;
```

Pure operations, stack operations, and control flow are emitted directly.
Environment-dependent operations call:

```c
evm_word neverd_evm_host_op(
    struct neverd_evm_env *environment,
    uint8_t opcode,
    evm_word a0, evm_word a1, evm_word a2, evm_word a3,
    evm_word a4, evm_word a5, evm_word a6);

void neverd_evm_trace(
    struct neverd_evm_env *environment, uint64_t pc, uint8_t opcode);
```

Arguments are in EVM pop order: `a0` is the original stack top. The callback
implements memory, storage, calldata, hashing, block context, calls, logs, and
halt side effects in `environment`; its return value is the opcode's first
pushed value. Unused arguments are zero. `neverd_evm_trace` runs before each
decoded instruction.

Compile with a Clang target whose frontend supports at least 512-bit `_BitInt`:

```bash
clang -std=c2x -ffreestanding -c contract.c
```

Apple's Darwin Clang target currently caps `_BitInt` below the required width.
On macOS, use a capable non-Darwin target for source validation or consume
NeverD's LLVM output directly; Linux Clang targets support the emitted widths.

## Generated Solidity contract

Solidity output contains two complementary views:

1. recovered selector-specific function, storage, event, and error declarations
   for audit readability; and
2. a checked PC/stack state machine preserving exact decoded arithmetic and
   control flow.

Recovered constant storage facts are emitted as named absolute slot constants,
for example `recovered_storage_slot_3 = uint256(0x3)`. They are not emitted as
ordinary sequential Solidity state variables, which would invent a false
storage layout.

The contract is intentionally `abstract`. Override `_evmHost` to implement
environmental operations. Its `args_[0]` is the original stack top, its return
value is the first pushed result, and it may update storage or other state.
`_evmTrace` is virtual and emits `EVMTrace` by default. This boundary makes
environment assumptions explicit instead of inventing Solidity source that
was not recoverable from bytecode.

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

`neverd_decompile_all` remains backward compatible and emits C. New EVM-aware
metadata and configuration entry points are `neverd_session_bitness`,
`neverd_evm_set_strict`, `neverd_evm_set_hardfork`, and
`neverd_decompile_all_ex`. Requesting Solidity for a native binary returns an
explicit unsupported-language error. The legacy LLVM-to-C routing flag is
rejected for EVM instead of being silently ignored: use the dedicated C23
backend for C source, or the `lift`/LLVM query APIs for verified LLVM IR.
The native object-code round-trip API also rejects EVM explicitly: LLVM IR is
available for analysis, but NeverD does not pretend that a native object target
provides an EVM ABI.

## Explicit limitations

- Legacy bytecode only; EOF containers are not decoded yet.
- Amsterdam/Bogota are explicit development targets; `latest` remains the
  finalized Fusaka instruction set until the scheduled opcodes are final.
- No RPC fetching, chain-state discovery, gas accounting/refunds, or precompile
  execution. Calls and environment values are represented by deterministic
  interpreter fields or backend host hooks.
- Creation-code extraction recognizes common static wrappers; it is not a full
  constructor transaction emulator.
- Dynamic jumps remain explicit indirect CFG edges unless a bounded constant
  analysis proves a valid `JUMPDEST`; unresolved reachable jumps also make
  recovered source mutability conservative.
- ABI types, source names, mappings, events, and custom errors are best-effort
  recovery facts. NeverD does not claim original-source identity.
- The C and Solidity environment hooks must be implemented for standalone
  execution of contracts that use memory, storage, calldata, calls, logs,
  hashing, or blockchain context.
