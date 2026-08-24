**Languages**: [English](evm.md) | [简体中文](evm.zh-CN.md) | [繁體中文](evm.zh-TW.md) | [日本語](evm.ja.md) | [한국어](evm.ko.md) | [Français](evm.fr.md) | [Deutsch](evm.de.md) | [Español](evm.es.md) | [Italiano](evm.it.md) | [Русский](evm.ru.md) | [العربية](evm.ar.md)

# EVM decompilation

[← Documentation index](README.md)

NeverD loads legacy Ethereum Virtual Machine bytecode, builds dedicated
256-bit LowIR, stack-SSA MedIR, and recovered HighIR representations, and can
emit LLVM IR, C23, or Solidity. Strict analysis is the default, but legacy EVM
has no whole-image opcode validation: only a definitely reachable execution
lane that reaches an unassigned or fork-inactive opcode is rejected at that
opcode's exact program counter. Dead bytes and merely `MayReachable` CFG
candidates do not turn into strict errors.

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
and an opcode on another cannot move the boundary. A present
`deployedBytecode` or `runtimeBytecode` field is authoritative: an explicit
`0x` value is accepted as an empty, naturally stopped runtime and deliberately
prevents fallback to creation bytecode. A missing field may fall through to the
next candidate, while missing or whitespace-only hex without the explicit
prefix is rejected. Explicit raw input may likewise be empty.

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
multi-contract artifacts, invalid metadata bounds, and missing or blank hex
produce actionable errors. An explicit empty raw input or `0x` runtime remains
a valid empty program. The C++ loader API can select a contract from a
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
[Fusaka checkpoint 2](https://blog.ethereum.org/2025/04/29/checkpoint-2).
EOFv1/EIP-7692 is unscheduled, while the container proposal
[EIP-3540](https://eips.ethereum.org/EIPS/eip-3540) is Stagnant. The old
`execution-spec-tests` repository has been archived and its maintained tests
moved to
[execution-specs](https://github.com/ethereum/execution-specs/tree/master/tests).
NeverD therefore does not accept an experimental EOF container as finalized
mainnet behavior.

Strict mode rejects an unknown or fork-inactive byte only when a definitely
`Reachable` state lane proves execution reaches it. `--evm-relaxed` retains
such bytes as typed fault prefixes and diagnostics, and generated backends
still fault on execution; relaxed mode never treats an unknown byte as a NOP.

## LLVM-style metadata architecture

Hand-maintained EVM metadata follows LLVM's multiply-included `.def` pattern:

- `EVMOpcodes.def` is the single source of truth for every finalized legacy and
  opt-in development-fork opcode: encoding, actual pop/push mutations,
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
  sole go-ethereum naming alias plus deliberate historical and unscheduled-EOF
  exclusions. The orthogonal `EVMUpstreamSemanticsPolicy.def` maps NeverD forks
  to go-ethereum `params.Rules`, names exceptional base-stack prechecks, and
  classifies dynamic-immediate stack instructions. The upstream audit rejects
  byte, activation, `base_min_stack`, and `net_stack_delta` drift as well as
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
  stores each canonical function spelling and selector once, then declares
  separate per-standard `KnownFunctionVariantInfo` memberships with their
  return lists and independent/non-independent evidence role. Shared ERC-20 /
  ERC-721 spellings therefore remain one callable candidate without becoming
  independent evidence for either standard or inheriting the first variant's
  return type. Events and custom errors retain their distinct typed records.
- `EVMAnalysisLimits.def`, `EVMInterpreterLimits.def`,
  `EVMABIParserLimits.def`, and `EVMABITableLimits.def` declare their
  stage-specific analysis, interpreter, parser, and public-table ceilings.
  `EVMConstants.h` owns shared protocol widths and stable internal names and
  materializes the analysis defaults and diagnostic option names from
  `EVMAnalysisLimits.def`; the interpreter and ABI headers materialize the
  limits declared by their own tables.
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
  validated `JUMPDEST` targets, and path-sensitive whole-stack state lanes.
  Values are constants, producer-identified symbols, hash-consed expressions,
  finite constant sets with explicit exactness, or top. Correlated operands
  stay in the same lane, so a Cartesian product from unrelated paths cannot
  manufacture a definite jump target. `MayReachable` indirect edges preserve
  conservative CFG candidates, but never authorize definite semantic facts.

  Every instruction, block, state, value, stack, edge, worklist update, and
  instruction-by-lane transfer is charged to a named `AnalyzeOptions` limit,
  including `MaxAbstractValuesPerSlot`, `MaxStackHeightVariants`, and
  `MaxAbstractInstructionTransfers`. Zero or exhaustion is a hard analysis
  error before insertion. Loop recurrence is a semantic abstraction, not a resource escape:
  on a back-edge, a changed loop-carried slot becomes `Top` so the fixed point
  converges. Budget exhaustion never requests any additional emergency widening
  and instead returns a hard error. Exact invalid targets still fail at their
  jump PC. In relaxed mode, stack faults terminate only their own lanes; no
  impossible post-fault fallthrough is fabricated.

  `EVMLowFaultKinds.def::InvalidJumpDestination` is path-sensitive at an
  `end-of-code JUMPI`: a definitely true condition with an invalid target has
  no successful tail and records a definite fault; a definitely false condition
  succeeds. An unknown condition retains only its possible successful false
  path and does not mislabel the entire lane as a definite fault.
- **EVM MedIR** keeps one explicit SSA execution lane for each LowIR state lane
  instead of merging incompatible stacks by maximum height. Each operation
  records its executing and faulting lanes, and phis retain source-lane
  identity. Its private constant lattice is `Uninitialized`, one exact
  `Constant`, or `Overdefined`: equal constants propagate across blocks and
  anchored phi cycles, while incomplete, conflicting, or runtime-dependent
  inputs cannot fabricate a constant. Checked def-use IDs and independent
  budgets for values, state lanes, stack entries, operations, operation-lane
  references, phi incomings, and worklist updates make malformed graphs and
  exhaustion fail loudly. The shared `Semantics.h` evaluator keeps constant
  folding aligned with interpretation. MedIR also preserves the primary effect plus orthogonal
  `none/read/write/readwrite` EVM-memory, source-level state, and call-value
  access.
- **EVM HighIR** recovers Solidity dispatcher selectors, likely calldata and
  return words, mutability, constant storage slots, LOG/event facts, revert
  facts, and function/CFG regions. A checked producer index and iterative,
  memoized value walk recover facts from typed MedIR operands, not instruction
  distance: selector comparisons may cross blocks and phis, use either `EQ`
  operand order, and retain a derived 32-bit mask; argument offsets, storage
  keys, event topic0, non-payable and receive guards, and exact 32-byte return
  sizes use their semantic inputs. The iterative walk is structurally bounded
  by the MedIR graph and treats malformed, mixed, or cyclic expressions as
  unknown. Conflicting dispatcher targets or incompatible ABI evidence for the
  same selector are diagnosed and omitted. Selector discovery starts from the
  root lane and follows the dispatcher's unmatched edges; selector-like tests
  nested inside a handler are not promoted to public functions. Receive and
  fallback recovery is root-constrained too and requires a definitely reachable
  successful terminal; reverting, faulting, non-payable empty-calldata, and
  merely possible paths cannot establish either entry point.

  The byte-granular, flow-sensitive
  memory analysis follows constant-offset writes across blocks, composes
  overlaps with byte-level kill/gen semantics, and invalidates knowledge on an
  unknown write. Proven payload recovery currently covers selectors and known
  Panic bytes. For a known custom-error declaration, the Solidity emitter keeps
  its canonical parameter types; NeverD does not claim to recover every runtime
  argument value. Solidity payability remains independent from the state-access
  lattice, and a reachable unresolved dynamic jump forces conservative
  `nonpayable` recovery.

  A selector, event topic, or familiar signature is only an evidence-backed
  candidate. A canonical function candidate is rejected by contradictory
  calldata use; shared selectors contribute no independent standard evidence.
  A standard needs its configured number of independent compatible selectors
  or strong exact event-topic/arity, storage-slot, or proxy evidence. Only then
  can a per-standard function variant be selected. Its static return list is
  emitted only when every definitely reachable successful terminal agrees on
  the exact ABI byte count; unresolved transfers, conflicting success shapes,
  or a mismatch fail closed without borrowing a dictionary return type.
  Reverts and faults are not successful returns. Event names likewise require a
  compatible indexed-topic count, and ambiguous signatures stay unnamed.
  Standard labels are conservative observations, not claims of original
  source, complete ABI recovery, or full ERC compliance.

  HighIR has separate hostile-input budgets for functions, lane and operation
  visits, region block references, memory read requests, tracked bytes, memory
  state cells, and memory worklist updates. The memory fixed point consumes only
  definitely reachable executing lanes, meets predecessor bytes by consensus,
  and returns a hard error on budget exhaustion rather than truncating facts.

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

Before any opcode-specific side effect, the interpreter preflights the typed
required stack height, pop count, and retained-plus-pushed height, so underflow
or overflow cannot partially execute an instruction. `EVMForkSemantics.def`
selects the meaning of byte `0x44`: `DIFFICULTY` before Paris and `PREVRANDAO`
from Paris onward. `REVERT`, semantic faults, step limits, and allocation/length
resource exhaustion restore storage, transient storage, logs, and selfdestruct
effects to the entry snapshot while retaining frame-local diagnostics and
explicit revert bytes. Allocation failure is reported as
`ExecutionFaultKind::ResourceExhausted` without requiring an error-string
allocation; if even the entry snapshot could not be materialized,
`HasPersistentStateSnapshot` is false and the result is never committable.

### Public IR and resource boundaries

Public `execute` first verifies that
`Code`/`Fork`/`Instructions`/`JumpDestinations` form canonical LowIR. A changed
fork, forged instruction record, inconsistent encoding, or jump-destination
table therefore returns `llvm::Error` before execution indexes the instruction
table. Public `lowerToMedIR` likewise validates every configured option,
resource bound, and structural invariant—in that order—before a
`canonical decode replay` decodes `Low.Code` under the embedded fork/strictness
and compares every LowIR field. Only then may it call
`lowerCanonicalLowToMedIR`, build an index, or allocate output proportional to
caller-controlled records. Public `recoverHighIR` similarly replay-validates
external LowIR/MedIR. The private `lowerCanonicalLowToMedIR` and
`recoverCanonicalHighIR` paths are reserved for IR owned by `analyze`; they skip
only redundant, non-recursive replay, while HighIR option/resource budgets
remain mandatory.

Dispatcher proof keeps one sorted `Any/Exact/Excluded` selector domain per
`MedStateLane`. Joins union exact sets, intersect exclusion sets, and remove an
exact set from a cofinite exclusion set; widening a domain requeues the lane.
An equality records its true-edge candidate only when the selector is allowed
and excludes it on the false edge. Raw `XOR(selector, constant)` records the
zero/false match when every canonical successor names the same entry; this
fallthrough form need not target `JUMPDEST`. Its nonzero/true mismatch edge
excludes that selector, while `ISZERO` converts the same expression to equality.
Selector-word, zero-calldata-word, calldata-size, and call-value guards refine
their individual edges. An unknown conditional stops the dispatcher proof
instead of exploring a merely possible branch.

After a function candidate is recognized, function-scope traversal continues
with that candidate's `exact singleton selector`. If control jumps back into a
shared dispatcher, `SelectorEquality`, raw `XOR`, and `SelectorWord` conditions
take only the `definite edge` consistent with the already matched selector.
Unknown or unrelated predicates conservatively retain all `definite edges`.
The traversal never uses an “exclude other entry blocks” heuristic: legitimate
`shared body/tail-call` control flow remains part of the function scope.

External CALL/CREATE outcomes are different: the host result is genuinely
nondeterministic, so analysis explores both precise CFG outcomes. This retains
ERC-1167 fallback recovery without treating an unreadable selector condition
as evidence; a genuinely unknown dispatcher still fails closed.

`EVMAnalysisLimits.def` gives the linear decoder and CFG builder one aggregate
LowIR diagnostic budget through `MaxLowDiagnostics` and
`MaxLowDiagnosticBytes`. Both paths precharge exact count and final bytes, and
reject zero limits. Low and High diagnostic budgets remain independent. The
same table independently charges
`MaxHighDispatchCandidates`, aggregate `MaxHighRecoveredArguments`,
`MaxHighDiagnostics` plus `MaxHighDiagnosticBytes`,
`MaxHighReferenceVisits`, `MaxHighMemoryTransferCells`, and
`MaxHighMemoryValueVisits`. Candidate and recovered-argument records are
precharged before either destination container or any name/type allocation.
Every HighIR output diagnostic is charged by count and final message bytes
before construction or copying, including the fixed malformed-IR diagnostic;
an exhausted diagnostic budget returns its named hard error rather than
silently omitting the diagnostic or facts.
The default root CFG region charges `MaxHighRegionBlockReferences` before
reserving or copying its block-PC list.

`EVMABIParserLimits.def` bounds tuple nesting, type nodes, and aggregate array
dimensions. `EVMABITableLimits.def` bounds public signature/variant table
cardinality and aggregate text. Public table validation applies those bounds
before parsing or hashing and then rejects invalid enum values, kind metadata,
standards, selector-evidence roles, canonical types, derived hashes,
memberships, and collisions. Production selector lookup is indexed, event
lookup uses a sorted topic table, and topic APIs verify an `APInt` is exactly
one EVM word before comparison or ordering.

`EVMInterpreterLimits.def` declares `MaxSteps`, `MaxMemoryBytes`,
`MaxTraceEntries`, `MaxLogEntries`, aggregate `MaxLogDataBytes`, aggregate
`MaxHostReturnDataBytes`, `MaxCalldataBytes`, aggregate
`MaxHostEnvironmentEntries`, aggregate `MaxExternalCodeBytes`, and
`MaxPersistentStateEntries`. The host-entry aggregate spans `BlockHashes`,
`Balances`, `CodeHashes`, `ExternalCode`, and `BlobHashes`; the external-code
byte limit spans every `ExternalCode` body. `MaxSteps` retains
its explicit `StepLimit` result. Runtime memory, trace, log, log-data, and new
persistent-state growth are precharged; exceeding those configured ceilings
returns `ResourceExhausted` and rolls back persistent state, logs, and
selfdestruct effects. An oversized initial host return-data aggregate or
persistent-state map is instead an `execute` API error. The interpreter keeps
host return data as `ArrayRef` views and uses `lower_bound` over the already
validated sorted instruction table rather than copying buffers or rebuilding a
per-execution PC map. The `const execute preflight` validates program and all
host-input limits before copying the environment, taking a persistent-state
snapshot, or constructing a result.

### Live go-ethereum differential audit

The standard local and CI audit always runs `git fetch --depth=1 --force` for
the official `https://github.com/ethereum/go-ethereum.git` default branch's
remote `HEAD`. Each run creates an unpredictable private temporary bare repository;
there is no shared persistent Git repository or cache. Only the authority ref
returned by that fetch and its resolved exact SHA select the revision. The SHA
is reported and probed in a detached temporary worktree, then the authority
repository and worktree are destroyed together. Neither
`local_docs`, an existing checkout, nor a submodule is an audit path; a pinned
submodule would be stale precisely when the live drift check is needed.

Every Git command first clears all inherited `GIT_*`, including
`GIT_CONFIG_*`, then installs only audited settings. `GIT_CONFIG_NOSYSTEM`
and `GIT_CONFIG_GLOBAL` disable system/global configuration;
`GIT_ATTR_NOSYSTEM` and command-scoped `core.attributesFile` disable
system/global attributes; command-scoped `core.hooksPath` disables hooks. The
private repository rejects unexpected local
configuration, grafts, `objects/info/alternates`, and `refs/replace`;
`GIT_NO_REPLACE_OBJECTS` also disables replacement lookup. Any violation
fails closed.

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

The public CLI exposes only `--manifest-output`; source, ref, and toolchain are
not caller-selectable. Its `schema 3` manifest is a closed record of source
identity, canonical and live-mainnet table evidence, dynamic-family evidence,
and diagnostics. The Go probe reflects the complete exported boolean-field
inventory of `params.Rules`, calls `LookupInstructionSet(params.Rules)` for
every mapped fork, and scans all 256 byte slots so an unreviewed activation
cannot hide outside NeverD's request. Allocation is determined from geth's
`operation.undefined`; `HasCost` is only a cost cross-check because it is also
false for defined zero-cost operations. Every `defined && !HasCost` slot must
match `EVM_GETH_ACTIVE_WITHOUT_COST` exactly at its declared activation fork;
an undefined slot with cost, an unreviewed defined slot, or an upstream change
that hides the marker fails closed. Unknown, duplicate, missing, out-of-range,
or unparsed fields, forks, names, bytes, and records are errors. Every `.def parser`
also rejects unconsumed macro-looking input instead of silently accepting a
partial policy. `EVMUpstreamOpcodePolicy.def` owns aliases and typed reviewed
historical/unscheduled-EOF exclusions, whose overlap/inactive invariants are
validated. The orthogonal `EVMUpstreamSemanticsPolicy.def` owns the closed
`params.Rules` reflection inventory, fork mapping, base-stack exceptions, and
dynamic-immediate families. CI runs this job on pushes to `dev`, pull requests,
manual dispatch, and the daily schedule, and uploads the exact revision,
manifest, and log as a failure artifact.

More specifically, `EVMUpstreamSemanticsPolicy.def` gives every exported
boolean `params.Rules` field exactly one `EVM_GETH_RULE_FIELD` entry in one of
three categories: `MappedForkSelector`, `NoOpcodeAllocation`, or
`ExcludedSelectorExpectedError`. The audit enables each field alone and calls
`LookupInstructionSet`: the first two categories require a nil error and the
third requires an error, while the returned complete 256-slot opcode/stack
fingerprint must always equal its `ExpectedFork`. The currently reviewed
no-allocation fields `IsEIP155`, `IsEIP2929`, `IsEIP4762`, and `IsPetersburg`
fingerprint as Frontier; `IsUBT` is expected to error and return the Cancun
fingerprint.

`EVMUpstreamSemanticsPolicy.def` declares which opcodes form each EIP-8024
dynamic family and the family's operation kind and valid stack delta;
`EVMEIP8024Immediates.def` remains the sole immediate-decoding authority. Its
single- and pair-operand inventories each classify all 256 byte values
explicitly as valid or invalid, and production decoding uses a direct table
lookup. The fresh-geth audit uses `go -overlay` to inject a virtual wrapper into
`core/vm` and obtains the real private `operation.execute` handlers from each
audited jump table. It covers the real `canonical fork jump tables` and the
`mainnet active/scheduled jump tables` table by table. An `inactive` declared
family is recorded explicitly; a `partial` family is an error. For every table
where the family is active, the probe executes the three declared operations
across all immediate bytes (`3x256`) plus `3 missing-operand cases`. It checks
acceptance, PC delta, unique-marker-derived stack operands and mutation, exact
valid-case underflow, and the missing operand's `0x00` behavior. Python compares
every observation with the same two declarative policies; it does not duplicate
the decoding formula.

There is exactly one canonical target of `EVM_HARDFORK_LATEST`.
`EVMUpstreamForkAliases.def` is the closed map for geth's `LatestFork` names:
Prague maps to Pectra; Osaka and BPO1 through BPO5 map to Fusaka; Paris,
Shanghai, Cancun, Amsterdam, and Bogota are identities. An unknown new name
fails closed. Each audit fixes and records one `audit_unix_time`, requires
`MainnetChainConfig.LatestFork(time)` to map to NeverD's latest fork, and
requires `LatestFork(max uint64)` to be in the alias inventory with its
canonical fork already probed; both instruction sets receive a complete table
comparison. The manifest records `authority=official-fresh-fetch`, the official
URL, requested `HEAD`, and resolved SHA. The probe fixes `GOTOOLCHAIN=local`.

Both sides bound hostile metadata before materializing it. Go and Python apply
`input/collection/string hard limits`; oversized JSON, arrays, or text fail
closed. They also enforce `bounded diagnostic output`: an overlong displayed
diagnostic carries its full-content `digest` and an `explicit truncated marker`
instead of being mistaken for the complete message. Every child command runs
with bounded output and a deadline. Timeout or output-limit failure kills the
entire `process group`/process tree and drains its pipes, so a grandchild cannot
outlive the audit or hold a descriptor indefinitely.

The current schema-3 live receipt records `schema_version=3`,
`audit_unix_time=1787534659`, `authority=official-fresh-fetch`,
`remote=https://github.com/ethereum/go-ethereum.git`, `ref=HEAD`, revision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562`, local `Go 1.24.0`,
`stack_limit=1024`, and `diagnostics=[]`. It compares `21 fork tables` and
`20 Rules probes` classified as `15 mapped/4 no-op/1 expected-error`. Both
`mainnet active/scheduled` records name `upstream BPO2`, which the closed alias
maps to `NeverD Fusaka`. EIP-8024 covers `23 table targets`; only
`Amsterdam/Bogota` are active, producing `1536 candidate executions` and
`6 missing-operand cases`, and the `three handler symbols` match across both
active targets. The closing tests are Python audit `67/67` and
`C++ Opcode 10/10`. On macOS, `sandbox-exec` completed the real audit with
network disabled for the final `go run`; the Linux workflow requires
`bubblewrap`.

Every Go phase—`go env`, `go mod init`, `go mod edit`, `go mod tidy`,
`go mod download`, and `go run`—runs inside a capability-root filesystem
sandbox. Read capability is limited to the private probe, fresh geth worktree,
validated `resolved GOROOT`, and exact required system runtime roots; only the
isolated environment roots are writable. Network capability is added only to
the dependency stages that need it, and the final run is offline. Sentinels in
the `host HOME/workspace` are denied and their contents cannot appear in
output. Linux uses an isomorphic `bubblewrap` policy with no `/` broad bind.

`NeverDEVMDecoderPropertyTests` independently exhausts every two-byte input at
each decoder-changing fork and checks complete instruction/JUMPDEST boundaries;
it also feeds deterministic hostile byte strings through every fork with a
bounded input size. This complements, rather than substitutes for, the live
client comparison.

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

- Legacy bytecode only; experimental EOFv1/EIP-7692 containers are not decoded
  as finalized mainnet instructions, and EIP-3540 remains Stagnant.
- Amsterdam/Bogota are explicit development targets; `latest` remains the
  finalized Fusaka instruction set until the scheduled opcodes are final.
- No RPC fetching, chain-state discovery, gas accounting/refunds, or precompile
  execution. Calls and environment values are represented by deterministic
  interpreter fields or backend host hooks.
- Creation-code extraction recognizes common static wrappers; it is not a full
  constructor transaction emulator.
- Dynamic jumps remain explicit indirect CFG edges unless exact lane evidence
  proves a valid `JUMPDEST`; may-reachable candidates never create definite
  HighIR facts, and unresolved reachable jumps keep source recovery
  conservative.
- ABI types, source names, mappings, events, and custom errors are best-effort
  recovery facts. NeverD does not claim original-source identity.
- The C and Solidity environment hooks must be implemented for standalone
  execution of contracts that use memory, storage, calldata, calls, logs,
  hashing, or blockchain context.
