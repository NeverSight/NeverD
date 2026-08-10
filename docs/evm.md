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
A compiler artifact field containing only an optional `0x` prefix is treated
as empty, so an empty `deployedBytecode` or `runtimeBytecode` field cannot hide
a usable creation-bytecode fallback.
A trailing Solidity CBOR map is stripped only when its encoded length, CBOR
map marker, and a known `solc`, `ipfs`, or Swarm key all validate.

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

The assigned legacy opcode set is covered from Frontier through Fusaka,
including `PUSH0`, transient storage, `MCOPY`, blob opcodes, and `CLZ`. The
default `latest` target is Fusaka. Accepted names are:

```text
frontier, homestead, dao-fork, tangerine-whistle, spurious-dragon,
byzantium, constantinople, petersburg, istanbul, muir-glacier, berlin,
london, arrow-glacier, gray-glacier, paris, shanghai, cancun, pectra,
fusaka, latest
```

Common aliases are accepted: `dao`, underscore spellings such as
`tangerine_whistle`, `merge`, `prague`, and `osaka`. `latest` and `osaka`
currently resolve to the canonical `fusaka` execution revision.

`latest` deliberately means the latest finalized mainnet revision implemented
by NeverD, not the tip of Ethereum's development branch. Ethereum currently
describes [Glamsterdam](https://ethereum.org/roadmap/glamsterdam/) as an
upcoming Q4 2026 upgrade. Its scheduled-but-still-Review-stage
[SLOTNUM](https://eips.ethereum.org/EIPS/eip-7843),
[DUPN/SWAPN/EXCHANGE](https://eips.ethereum.org/EIPS/eip-8024) instructions
remain outside the default table until the fork and encodings are finalized.
This matters especially for EIP-8024: its immediate byte deliberately has
different `JUMPDEST` masking rules from `PUSH`, so pretending it is an ordinary
one-byte immediate would be backwards-incompatible.

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

- `EVMOpcodes.def` is the single source of truth for the 150 assigned legacy
  opcodes: encoding, complete stack contract, immediate width, opcode class,
  activation fork, primary effect, orthogonal EVM-memory access,
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
- `EVMHardforks.def`, `EVMEffects.def`, `EVMExitStatuses.def`, and
  `OutputLanguages.def` provide the corresponding ordered enums, parsers,
  display names, CLI choices, and C ABI values.
- `EVMConstants.h` owns protocol widths, limits, and stable default names.
- `Semantics.h` owns the target-independent scalar ALU evaluator. Constant
  folding and interpretation call the same checked `APInt` implementation;
  LLVM, C, and Solidity retain explicit target lowerings so backend contracts
  and unsupported cases remain visible.

The decoder is the raw-byte boundary. Assigned identity and hardfork activation
are deliberately separate: relaxed decoding preserves an assigned instruction's
name, introduction fork, and immediate width even when it is inactive for the
selected historical fork, but its semantic queries remain conservative and
faulting. This prevents an inactive immediate-bearing opcode from shifting all
later byte boundaries or accidentally acquiring current semantics. Analysis,
interpretation, and all emitters otherwise use the generated `Opcode` enum and
metadata queries. Raw encodings reappear only at byte-oriented ABI boundaries,
such as tracing and host callbacks. `SWAP16` has 17 logical stack inputs, while
the largest non-stack host operation has seven arguments; these are separate,
compile-time-derived limits.

`OpcodeInfo` cannot be default-constructed into a half-valid record, and its
name is an `llvm::StringLiteral`, not a potentially dangling `StringRef`. A
compile-time table validator rejects duplicate encodings, unknown classes or
properties on assigned rows, invalid scalar ALU contracts,
effect/state-access mismatches, incorrect PUSH/DUP/SWAP/LOG family contracts,
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

- **EVM LowIR** preserves PC, encoding, PUSH immediates (including truncated
  right-zero padding), basic blocks, predecessor/successor edges, validated
  `JUMPDEST` targets, reachability, and stack heights.
- **EVM MedIR** represents every stack value as a 256-bit SSA value, creates
  merge phis, constant-folds pure operations, and preserves both the primary
  semantic effect, orthogonal `none/read/write/readwrite` EVM-memory access,
  source-level state-access constraint, and call-value access. This keeps
  compound instructions honest for later dataflow, alias, mutability, and
  payability work.
- **EVM HighIR** recovers Solidity dispatcher selectors, likely calldata words,
  return words, mutability, constant storage slots, LOG/event facts, revert
  facts, and function/CFG regions. Recovered names and types are explicitly
  heuristic. Solidity payability is combined independently from the state
  access lattice because `payable` is not a stronger form of `view`; an
  unguarded `CALLVALUE` effect therefore dominates the recovered declaration,
  while a proven non-payable guard does not contaminate body mutability. A
  reachable unresolved dynamic jump joins state access with
  `Unknown`, so recovered Solidity falls back to `nonpayable` instead of making
  an unsound `pure` or `view` promise. Conflicting dispatcher patterns for the
  same selector are diagnosed and omitted instead of allowing one entry point
  to overwrite another.
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
- Review-stage Amsterdam opcodes are not enabled; `latest` currently selects
  the finalized Fusaka instruction set.
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
