**Languages**: [English](sbf.md) | [简体中文](sbf.zh-CN.md) | [繁體中文](sbf.zh-TW.md) | [日本語](sbf.ja.md) | [한국어](sbf.ko.md) | [Français](sbf.fr.md) | [Deutsch](sbf.de.md) | [Español](sbf.es.md) | [Italiano](sbf.it.md) | [Русский](sbf.ru.md) | [العربية](sbf.ar.md)

# Solana SBF decompilation

[← Documentation Index](README.md)

NeverD loads Solana deploy artifacts as first-class SBF programs and exposes
the complete path through the CLI and `libneverd`:

```text
SBF ELF
  → version-aware ELF loader and verifier
  → lossless LowIR + CFG
  → normalized MedIR + register facts
  → recovered functions, syscalls, CPI/account observations, and regions
  → Solana program facts: program id, Anchor dispatch, addresses, lints
       ├─ verified LLVM IR
       ├─ portable C11
       └─ safe stable Rust
```

The implementation follows the current Anza `sbpf` VM rather than treating
Solana programs as generic Linux eBPF. Its version, opcode, syscall, relocation,
call-argument ABI, and protocol metadata live in the `.def` databases under
`include/neverd/sbf/`; loaders and backends consume generated typed tables
instead of duplicating encodings or spellings.

The closed tables include `SBFVersions.def`, `SBFVersionFeatures.def`,
`SBFOpcodes.def`, `SBFRelocations.def`, `SBFArgumentRegisters.def`,
`SBFProtocolLimits.def`, `SBFValidationRules.def`, `SBFFaultCodes.def`,
`SBFEdgeKinds.def`, `SBFSyscalls.def`, `SBFSyscallMemory.def`,
`SBFSyscallRegistration.def`,
`SBFSourceStatuses.def`, `SBFCPIABI.def`, `SBFProgramInstructions.def`,
`SBFKnownAddresses.def`, `SBFAnchorNamespaces.def`, `SBFAnchorNames.def`,
`SBFAccountLayout.def`, `SBFLints.def`, and `SBFUpstreamSources.def`; ordinary
one-use diagnostics and LLVM block names remain local, matching LLVM's own
`.def` policy. These are typed registries for versions/features, syscalls,
faults, CFG edges, and generated-source ABI status, not stringly typed switches.
`SBFFaultCodes.def` owns stable execution-fault values;
`SBFSourceStatuses.def` separately owns the generated-source host ABI.

`SBFProtocolLimits.def` makes the numeric contract unambiguous: the enforced
current program-account-data/decode ceiling is 10 MiB, exactly `10'485'760`
bytes. The 65,536-instruction value is retained only as historical provenance
and a regression-test datum; it is not an active execution or decode limit.

After relocation, one immutable, VM-addressed `ProgramImage` is the semantic
source of truth. The decoder, interpreter, string recovery, LLVM backend, and
C/Rust backends all consume that same image; there is no independent rodata or
text copy that can drift from loader semantics.

The legacy loader is raw-first: it preserves file offsets and exact ELF
relocation provenance, fixes relative CALLs, then applies raw relocations once
in ELF ordinal order before materializing VM regions. Its fail-closed error
precedence is equally deliberate: text file/VM identity, relative CALL target,
relocation kind/site, entrypoint, then packed read-only layout. File-offset to
VM mapping is gap-aware, so sparse holes never shift later bytes or become
invented readable memory. Strict v3+ loading instead trusts bounded program
headers for runtime mapping and treats section/symbol tables only as optional
debug enrichment.

## Supported input and VM versions

The input is an ELF64 little-endian Solana program (`.so`). NeverD supports the
two layouts used by the current VM:

| SBF version | ELF layout | Machine IDs | Important ISA behavior | Status |
|-------------|------------|-------------|------------------------|--------|
| v0 | legacy sections and relocations | `EM_BPF`, `EM_SBPF` | fixed frames with virtual gaps, LDDW, legacy memory opcodes | legacy |
| v1 | legacy sections and relocations | `EM_BPF`, `EM_SBPF` | manually adjusted stack frames | legacy |
| v2 | legacy sections and relocations | `EM_BPF`, `EM_SBPF` | PQR arithmetic, moved memory encodings, swapped immediate subtraction, source-register CALLX | legacy, non-monotonic |
| v3 | strict program headers, no dynamic relocations | `EM_BPF` | static syscalls/calls, JMP32, destination-register CALLX, bytecode at `0x100000000`, rodata at zero | current deployed toolchain format |
| v4 | strict program headers, no dynamic relocations | `EM_BPF` | v3 ISA plus the aligned memory-mapping contract | current upstream `sbpf`; cluster availability may vary |

The default `RuntimeVersionPolicy::ChainProfile` follows the pinned Agave
runtime at the selected cluster and slot. The `SBF_RUNTIME_VERSION` rows bind
V1, V2, and V3 to their official enable-feature accounts, so a historical
profile advances its maximum ISA from V0 to V1, V2, then V3 as those gates
activate; the current maximum remains V3. An explicit `--sbf-version=v4`
request selects
`RuntimeVersionPolicy::UpstreamToolchain`, allowing expert offline analysis of
the v4 contract supported by pinned `sbpf` without pretending that Agave has
activated v4 on chain. Auto-detection remains chain-conservative; relaxed mode
changes diagnostics, not this provenance boundary.

The v2 changes intentionally do not leak into v3. Feature checks are explicit,
not `version >= N` guesses.

A version number is not itself a specification, so `SBFVersionFeatures.def`
holds the behavioural changes and the version table composes them. Each record
carries the SIMD proposal that accepted the change and the predicate
`anza-xyz/sbpf` exposes for the same question, because several proposals land
in one version and one proposal changes several unrelated things: SIMD-0173
both moves the memory instruction classes and retires `lddw`, while SIMD-0174
independently adds the PQR class in the same version. Recording the proposal on
the feature rather than on the version is what keeps a recovered version claim
traceable to the document that decided it, and it is why the two `callx` rules
are separate features: SIMD-0173 reads the source register and SIMD-0377 reads
the destination.

Strict mode is the default and rejects malformed
headers, ranges, alignments, unsupported writable legacy sections, invalid
continuations, bad registers, illegal frame-pointer writes, invalid branches,
and version-inactive opcodes with instruction slot and virtual address.

## The runtime a description is about

The ISA version comes from the file. Almost nothing else does. Which syscalls
resolve depends on the chain and the slot; which bytes an account field sits at
depends on the loader that owns the program; whether the entrypoint receives a
second argument depends on a switch the chain throws; and whether a program can
be deployed is a different question from whether it runs. A single version
switch cannot express any of that, so these are separate axes with separate
tables.

`SBFRuntimeFeatures.def` records clusters, purposes, and the gates that change
what NeverD reports, each with the runtime identifier, the feature account
whose state records activation, and the slot each cluster activated it at. A
pending account can exist without enabling its gate; a gate with no activation
row for a cluster has not been activated there. `simd-0321` is on for every
cluster; `simd-0449` and the SHA-512 syscall are on for testnet and devnet and
off for mainnet, which is exactly why a program that works on devnet fails on
mainnet.

At the pinned Agave revision, the `syscall_parameter_address_restrictions`
gate (`simd-0459`) tightens the VM-address and alignment contract for syscall
and CPI parameters; finalized RPC state records activation at slots 429,840,000
on mainnet, 407,468,256 on testnet, and 462,240,000 on devnet. The
`account_data_direct_mapping` gate changes account data from an input-buffer
copy to directly backed memory regions when the adjusted address space is in
use; it is not activated on mainnet and activates at slots 408,332,256 on
testnet and 463,968,000 on devnet. Neither gate creates a new account ABI or
changes logical ABIv0/ABIv1 field offsets: the owning loader still selects the
serialization, while NeverD records both gates as runtime topology metadata.

Feature bits are append-only. The observable snapshot now exceeds one 32-bit
word, so `RuntimeFeatureMask` is the single `uint64_t` storage and host-ABI
type. `RuntimeFeatureDisposition` keeps current `RuntimeBranch` use sites
separate from `FoldedBranch` rows whose active behaviour is unconditional in
the pin but whose pre-activation behaviour still matters for historical slots;
this prevents a historical fact from being advertised as a fake current Agave
branch. The finalized RPC audit recorded these loader, syscall, CPI, and VM
facts (`—` means no activation on that cluster):

| Feature gate | Domain / disposition | mainnet | testnet | devnet |
|--------------|----------------------|---------|---------|--------|
| `disable_deploy_of_alloc_free_syscall` | `ProgramAdmission` / `FoldedBranch` | 209,088,008 | 195,356,264 | 224,208,000 |
| `enable_bpf_loader_set_authority_checked_ix` | `LoaderManagement` / `RuntimeBranch` | 251,424,000 | 247,628,260 | 255,744,000 |
| `remove_bpf_loader_incorrect_program_id` | `LoaderManagement` / `FoldedBranch` | 237,168,000 | 224,300,256 | 247,104,000 |
| `simplify_alt_bn128_syscall_error_codes` | `SyscallSemantics` / `FoldedBranch` | 274,320,000 | 278,300,256 | 308,448,000 |
| `abort_on_invalid_curve` | `SyscallSemantics` / `RuntimeBranch` | 311,904,000 | 300,764,256 | 342,576,000 |
| `deplete_cu_meter_on_vm_failure` | `VMFaultPolicy` / `RuntimeBranch` | 327,888,000 | 319,340,257 | 364,176,000 |
| `fix_alt_bn128_multiplication_input_length` | `SyscallSemantics` / `FoldedBranch` | 361,152,000 | 346,988,256 | 397,440,000 |
| `raise_cpi_nesting_limit_to_8` | `CPIExecution` / `RuntimeBranch` | — | — | — |
| `increase_cpi_account_info_limit` | `CPIExecution` / `FoldedBranch` | 403,056,000 | 385,868,256 | 435,456,000 |
| `poseidon_enforce_padding` | `SyscallSemantics` / `FoldedBranch` | 406,080,000 | 385,868,256 | 438,048,000 |
| `fix_alt_bn128_pairing_length_check` | `SyscallSemantics` / `FoldedBranch` | 406,944,000 | 385,868,256 | 438,480,000 |
| `alt_bn128_little_endian` | `SyscallSemantics` / `RuntimeBranch` | 425,088,000 | 406,604,256 | 456,192,000 |
| `enable_alt_bn128_g2_syscalls` | `SyscallSemantics` / `RuntimeBranch` | 425,520,000 | 406,604,256 | 457,056,000 |
| `loader_v3_minimum_extend_program_size` | `LoaderManagement` / `RuntimeBranch` | 432,864,000 | 416,540,256 | 470,880,000 |

This scope deliberately does not claim the whole Agave `FeatureSnapshot`.
NeverD includes loader, verifier, VM, entry/input, syscall, and CPI
infrastructure gates only when they directly change decoding or the emitted
host contract. Transaction scheduling, fees, consensus, transaction-level
precompile verification, and `CPI target built-in` business semantics belong to
the `external runtime`; adding their bits without implementing those built-ins
would advertise a capability NeverD does not have.

`SBFLoaders.def` records ownership and serialization. Deploying and executing
stopped being the same answer years ago: `loader-v1` and `loader-v2` refuse
every management instruction they are sent and keep running the programs they
already own, which is why their serialization still has to be readable.

| Loader | Serialization | Deploys | Executes |
|--------|---------------|---------|----------|
| loader-v1 | `abi-v0` | no | yes |
| loader-v2 | `abi-v1` | no | yes |
| loader-v3 | `abi-v1` | yes | yes |
| loader-v4 | `abi-v1` | no | no (built-in removed) |

`SBFAccountLayout.def` places each account field under each serialization. The
two do not merely differ in padding — they order the fields differently, so at
offset three the unaligned form has the first byte of the account's address and
the aligned form has its executable flag, and nothing about the value announces
which one was read. A repeated account also occupies one byte in `abi-v0` and
eight in `abi-v1`, which misaligns a walk over the entries rather than a single
field.

Whether a call resolves is three questions, not one, so
`SBFSyscallLifecycle.def` holds how settled the published signature is and
`SBFSyscallRegistration.def` holds the rest: which registry a syscall appears
in, which gate governs it, and which way that gate points. Direction matters
because a gate can take something away as easily as add it — activating
`disable_fees_sysvar` is what removed the fees sysvar syscall — and reading a
removing gate as an adding one inverts the answer for every cluster at once.
`sol_alloc_free_` remains registered for execution on both sides of the
boundary. Deployment registered it before
`disable_deploy_of_alloc_free_syscall`, then rejects it at and after the
cluster-specific activation slot. The pinned Agave revision has folded the
active deployment side into its registry construction, but NeverD preserves
the gate so a historical profile gets the pre-activation answer.

On a runtime that has activated `simd-0321` the entrypoint also receives the
address of the instruction data in `r2`. NeverD models it as its own kind of
value rather than a constant, because where it lands depends on the accounts:
inventing an address would let a load through it be reported as a named account
field. Before activation the register arrives zero, and a program that reads it
reads a zero. Generated LLVM, C, and Rust entry points therefore take the input
buffer and the instruction data, because a callable that cannot be given the
second cannot reproduce a program that reads it.

The current Solana toolchain builds programs with `cargo build-sbf`. Modern
v3+ production programs are Rust-oriented; the upstream C toolchain does not
target v3. This does not limit NeverD's output backends: any accepted SBF input
can be rendered as either C or Rust.

Authoritative moving references:

- [Solana programs](https://solana.com/docs/core/programs)
- [Program execution](https://solana.com/docs/core/programs/program-execution)
- [Syscall reference](https://solana.com/docs/core/programs/syscall-reference)
- [Anza sbpf VM](https://github.com/anza-xyz/sbpf)
- [Agave changelog](https://github.com/anza-xyz/agave/blob/master/CHANGELOG.md)

## Loader trust boundary and runtime image

Strict v3/v4 loading trusts bounded executable program headers. Section and
symbol tables are optional debug enrichment: absent or malformed metadata is
reported with a typed enrichment status but cannot invalidate an otherwise
valid runtime image. This mirrors the runtime boundary and permits stripped or
sectionless programs without weakening instruction verification.

Legacy v0-v2 loading builds the upstream read-only image from `.text`,
`.rodata`, `.data.rel.ro`, and `.eh_frame`, with checked gaps and non-overlap.
`R_BPF_64_64`, `R_BPF_64_RELATIVE`, and `R_BPF_64_32` are applied exactly once
before the image becomes immutable. This includes the old non-text relative
relocation behavior required by deployed function-pointer/data fixtures.

## CLI

```bash
# Confirm the detected machine, version, layout, VM addresses, and sections.
neverd info program.so
neverd headers --json program.so

# Inspect every analysis stage. The high dump ends with the recovered Solana
# program facts: addresses, Anchor dispatch, CPI targets, account fields, lints.
neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

# Name instruction handlers from the program's own IDL instead of the built-in
# dictionary. The file is read locally; NeverD never fetches it.
neverd lift --dump-high --sbf-idl=program.json program.so

# Verified LLVM IR.
neverd lift -o program.ll program.so

# Both source backends are first-class.
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

# Decode a fixture with an explicit VM contract, or retain malformed input for
# forensic inspection. Auto-detection and strict verification are preferred.
neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so

# Say which runtime the answer is about. None of this is in the program file.
neverd lift --dump-high --sbf-cluster=devnet program.so
neverd lift --dump-high --sbf-slot=410400000 program.so
neverd lift --dump-high --sbf-loader=loader-v1 program.so
neverd lift --dump-high --sbf-purpose=deployment program.so
```

`--sbf-cluster`, `--sbf-slot`, `--sbf-loader`, and `--sbf-purpose` select the
runtime profile. The defaults describe mainnet-beta as it stands, under
`loader-v3`, for a program that is already deployed. Asking about deployment
instead reports the syscalls that would keep a program off the chain even
though the chain would keep running it.

`--sbf-version=auto|v0|v1|v2|v3|v4` changes instruction semantics after the
ELF has passed its detected-layout checks. It is intended for damaged or
research fixtures; it must not be used to reinterpret an untrusted file as a
different packaging standard.

## Analysis and recovery

LowIR retains each eight-byte encoding, raw fields, LDDW continuations,
resolved calls, syscall hashes, blocks, edges, reachability, and diagnostics.
MedIR normalizes version-specific encodings into typed 32/64-bit operations,
explicit immediate/result extension, guarded arithmetic, memory widths, and
call kinds. Register dataflow tracks constants plus stack/rodata addresses.

HighIR recovers entry/internal functions, direct call edges, official syscall
names, strings, natural loops, reducible conditionals, and conservative
Solana-specific observations. Calls to `sol_invoke_signed_rust` or
`sol_invoke_signed_c` are marked as CPI. Memory based on the input register is
marked as account/input access.

CFG ownership and dataflow are per-function: typed call edges never become
same-frame CFG predecessors, a tail reached from multiple function entries is
reported as shared rather than assigned by traversal order, and all latches of
one natural loop form one multi-latch region. Dependency worklists and flattened
ownership are guarded by 10,000-function, 10,000-reverse-block, and
10,000-conditional-latch fixtures; the contract is completion and linear-space
shape, not a machine-specific elapsed-time number.

Public SBF call graphs use `callgraph-budget=fail-closed`: typed input,
provenance, node, edge, element, and `CallGraphOutputByteBudget` limits make the
JSON exact-or-empty. Exhaustion returns `{"nodes":[],"edges":[]}` and sets
`neverd_last_error()`; it never publishes a partial relation.

## Solana program recovery

Above the SBF machine model, NeverD reports what a program means as a Solana
program. Every recorded fact carries the evidence that produced it, and anything
the bytes do not decide is left unset rather than guessed.

| Recovered | Evidence |
|-----------|----------|
| Base58 addresses in read-only data | `SBFKnownAddresses.def` match, or a constant the code materializes |
| The program's own declared address | a `sol_memcmp_` of exactly `kPubkeyByteCount` bytes against a read-only constant |
| Anchor instruction dispatch | a 64-bit comparison whose constant equals a namespaced SHA-256 discriminator |
| Cross-program invocation targets | the instruction record reachable from the invoke argument |
| The operation an invocation selects | a tabulated selector in `SBFProgramInstructions.def`, or a leading Anchor discriminator |
| Program-derived address seeds | the seed descriptor array reachable from the derivation argument |
| Account field reads and writes | a load or store whose address provably lands in the serialized input |

The loader passes one argument, the serialized input buffer at the base of the
input region, so constant propagation from that entry state gives named account
fields rather than raw offsets. `SBFAccountLayout.def` holds the official
serialization; its fixed fields are checked to tile their span exactly, so an
offset cannot silently drift.

### Scratch memory and syscall windows

A program almost never hands the runtime a constant. It assembles a seed array,
a serialized instruction, and that instruction's payload in its own frame or on
its heap, and passes a pointer. Reading only the loaded image would see the
pointer and nothing it addresses, so recovery carries a byte-accurate model of
the memory only this program can write, bounded by `kMaxModeledScratchBytes`.

Scratch recovery is demand-driven: the Solana CPI/PDA scratch fixed point is
constructed only when a real scratch consumer exists; programs without such a
consumer skip the whole-CFG fixed point. `SBFAnalysisLimits.def` defines host
analysis policy, not protocol limits: `MaxModeledScratchBytes` is 1,024 bytes
per program point and `ScratchFlowRetainedByteBudget` is an 8,388,608-byte
logical retained estimate. When the retained budget is exceeded, recovery
widens explicitly to `ScratchRecoveryPrecision::BlockLocal`. It discards only
cross-block must-facts; block-local replay remains sound and can still recover
same-block stores. The printer emits the stable line `recovery scratch-precision=block-local`, and widening never returns
half-converged must-facts.

Two facts decide what survives a call. `SBFSyscalls.def` says which argument
registers carry a VM address; `SBFSyscallMemory.def` says what the runtime does
through each of them, as a read or a write with a `Fixed`, `Counted`, or
`Opaque` extent. A syscall with no write window cannot change a caller byte, so
everything proven before `sol_log_` is still proven after it. A write bounded by
a length argument invalidates exactly that window. An `Opaque` write invalidates
its base address and everything above it, because a buffer never extends below
where it starts or across a VM region boundary. The effect summary in
`SBFSyscalls.def` and the window table are validated against each other in both
directions, so neither can drift alone.

`sol_memcpy_`, `sol_memmove_`, and `sol_memset_` are followed rather than merely
invalidated: with a proven destination, length, and source, the destination
bytes become known. That is what recovers the operation an Anchor program
invokes, since its payload is copied into place rather than mapped.

Only a resolved runtime syscall may preserve scratch, and then only according
to its audited write windows. Every internal, indirect, or otherwise unresolved
call clears the modeled bytes—even when no current argument points at
scratch—because an earlier escape or global alias can still let the callee
mutate them. `sol_invoke_signed_rust` and `sol_invoke_signed_c` write account
data rather than caller memory, so two invocations assembled in one block are
both readable.

The model is a forward must-analysis over the intra-function CFG: a byte
survives into a block only when every path that reaches it wrote the same value.
Call edges are not followed, because a callee inherits nothing from its caller's
frame. Its dependency worklist has no block-count precision escape hatch; an
opt-in release gate exercises the full 10 MiB, `1,310,720`-instruction ceiling.

Anchor derives a discriminator by hashing `<namespace>:<name>` with SHA-256 and
keeping the leading eight bytes, which is one-way. NeverD confirms candidates
instead: `SBFAnchorNames.def` is a dictionary of names that recur across
deployed programs, and `--sbf-idl` supplies the program's own IDL, which takes
precedence. Both the modern IDL layout with explicit `discriminator` arrays and
the legacy layout with derived discriminators are accepted; an entry that cannot
be matched is reported rather than dropped. A 64-bit comparison is only called a
discriminator once at least one of them resolves to a name, so an ordinary
constant comparison is never presented as an instruction.

`SBFKnownAddresses.def` records protocol and canonical-program addresses. Every
entry must decode to exactly 32 bytes, which the test suite enforces, so the
table validates its own spellings. The all-zero System Program address is only
recognized where code references it, because it would otherwise match every zero
run in read-only data.

Recovery needs the syscall ABI to read this correctly. SBPFv3 maps read-only
data at virtual address zero, so a length argument and a low data address are
the same number; `SBFSyscalls.def` therefore records which argument registers
carry a VM address, and only those are followed.

The two invocation syscalls describe the same instruction with two different
structures, and `SBFCPIABI.def` keys both layouts by the syscall that selects
them. Reading one with the other's offsets does not fail; it silently reports
the first account as the invoked program. `SBFProgramInstructions.def` then
names the operation a canonical program was asked for, from the selector its own
interface publishes: a bincode variant index for the system, stake, lookup-table
and upgradeable-loader programs, and a leading byte for the token programs,
including Token-2022's extension range on top of the numbering it shares with
the original token program. An unlisted selector is reported as its number.

`SBFLints.def` catalogs whole-program observations: a missing signer or owner
check, an invocation target that is not constant, a deprecated or feature-gated
syscall, and an SBPF version that SIMD-0500 will stop accepting for deployment.
Each carries a severity and a confidence, and the signer and owner lints stay
silent unless at least one account access was resolvable, because absence of
evidence is not evidence. A lint never changes decoded semantics.

Nothing in this layer contacts the network. Live IDL and account fetching remain
outside the tool.

C and Rust share one backend-neutral structuring pass. It emits direct
`if`/`if-else` plus natural `while`/`loop` constructs when every reachable
block has a unique reducible representation. Internal calls, CALLX, and
irreducible control flow conservatively retain the exact PC dispatcher, so
readability never changes execution semantics.

The syscall database includes logging, memory, PDA, SHA-256/Keccak/Blake3,
Poseidon, secp256k1, curve and alt-bn128 operations, big modular exponentiation,
CPI, return data, sibling instructions, compute-unit queries, and current
sysvars including epoch rewards. Each record carries exact register arity,
return kind, effects, availability, and provenance. Published feature-gated
entries remain distinct from stable entries; Agave-master-only SHA-512,
BLS12-381 decompression, and pairing metadata is not presented as cluster
stable. The audited Agave revision is stored in
`SBFUpstreamSources.def`, not embedded in backend code.

Legacy relocations `R_BPF_64_64`, `R_BPF_64_RELATIVE`, and `R_BPF_64_32` are
recognized centrally.
Text relocations are applied before decoding, including both halves of LDDW
addresses and the official Murmur3 CALL key written by the legacy VM loader.
When a legacy artifact has already applied and stripped `R_BPF_64_32`, NeverD
recomputes the official function-registry key from function symbols and target
slots to retain internal-call recovery.

## Generated LLVM runtime contract

Lifted LLVM never treats a VM address as a host pointer. Checked load, store,
and syscall declarations return an `i32` status; loads and syscalls write their
`i64` value through an output pointer. Every nonzero status branches to an
explicit SBF fault block before execution can continue. The resulting module
is passed through `llvm::verifyModule` before it leaves the backend. Runtime
declarations carry typed `nounwind`, `captures(none)`, and `writeonly`
attributes where their ABI permits it; fault callbacks are marked cold. Normal
programs retain one LLVM block per analyzed SBF basic block, while CALLX
programs add only the dynamic entry blocks required to model any valid raw
instruction address.

## Generated C host contract

The C backend emits portable C11 and a typed environment:

```c
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION = 1,
  NEVERD_SBF_MEMORY_ACCESS = 2,
  NEVERD_SBF_DIVIDE_BY_ZERO = 3,
  NEVERD_SBF_DIVIDE_OVERFLOW = 4,
  NEVERD_SBF_CALL_DEPTH = 5,
  NEVERD_SBF_UNKNOWN_SYSCALL = 6,
  NEVERD_SBF_UNKNOWN_FUNCTION = 7,
  NEVERD_SBF_EXECUTION_OVERRUN = 8,
} neverd_sbf_status;
/* v2 is fixed-width: values 0..8 reuse the legacy constants above. */
typedef uint32_t neverd_sbf_status_v2;
enum {
  NEVERD_SBF_INVALID_REGISTER = 9,
  NEVERD_SBF_INVALID_BRANCH = 10,
};
typedef uint64_t neverd_sbf_runtime_feature_mask;
typedef struct neverd_sbf_runtime_features {
  neverd_sbf_runtime_feature_mask bits;
} neverd_sbf_runtime_features;

/* Generated feature constants have the form NEVERD_SBF_RUNTIME_FEATURE_<Name>. */
typedef struct neverd_sbf_syscall_invocation {
  uint32_t hash;
  uint64_t arguments[5];
  neverd_sbf_runtime_features runtime_features;
} neverd_sbf_syscall_invocation;

/* v1 is the exact legacy four-field ABI. */
/* All callback fields return int, including the v2 callback. */
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  /* Legacy syscall callback: hash, five arguments, output value. */
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;

/* The v1 entrypoint reads only the four fields above. */
neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);

/* v2 is a distinct ABI: the old layout is embedded and never extended in place. */
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  /* NULL callback falls back to base.syscall. */
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *result);
  /* NULL selects the program snapshot; a pointer to zero is an explicit empty snapshot. */
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *env, uint64_t input,
    uint64_t instruction_data, uint64_t *result);
```

`width` is in bits. Every generated C callback returns `int`, including
`syscall_with_features`. For the v1 `neverd_sbf_program` entrypoint, zero
means success; any nonzero `load` or `store` return is normalized to
`NEVERD_SBF_MEMORY_ACCESS`, and any nonzero `syscall` return to
`NEVERD_SBF_UNKNOWN_SYSCALL`; v1 never propagates an exact callback status.
Internal `InvalidRegister` and `InvalidBranch` faults also normalize to
`NEVERD_SBF_INVALID_INSTRUCTION`.
The v2 `neverd_sbf_program_v2` entrypoint is the exact-status path: a
recognized `neverd_sbf_status_v2` callback value, including 9 or 10, is
preserved as the handled fault. The v2 entrypoint also preserves internal
`InvalidRegister` and `InvalidBranch` as 9 and 10. An unknown callback value
uses the generated operation-specific fallback. A null `syscall_with_features`
falls back to `base.syscall`; its callback still returns `int`.
The v1 struct and entrypoint remain compatible with legacy hosts. Use the
separate v2 entrypoint to receive `syscall_with_features` and the resolved
runtime-feature snapshot. Registers, return PCs, callee-saved r6-r9, frame
pointers, VM addresses, division faults, wide PQR operations, and wrapping
shifts are represented in the generated source. Only helpers actually used by
the program are emitted, so `clang -Wall -Wextra -Werror` accepts minimal
output.

## Generated Rust host contract

Rust output is stable, safe Rust and uses a trait rather than raw pointers:

```rust
// The v1 source contract remains Result-based.
pub enum SbfError {
    InvalidInstruction, MemoryAccess, DivideByZero, DivideOverflow,
    CallDepth, UnknownSyscall, UnknownFunction, ExecutionOverrun,
}

#[repr(u32)]
#[non_exhaustive]
pub enum SbfErrorV2 {
    InvalidInstruction = 0, MemoryAccess = 1, DivideByZero = 2,
    DivideOverflow = 3, CallDepth = 4, UnknownSyscall = 5,
    UnknownFunction = 6, ExecutionOverrun = 7, InvalidRegister = 8,
    InvalidBranch = 9,
}

pub struct SbfRuntimeFeatures { bits: u64 }
impl SbfRuntimeFeatures {
    pub const fn from_bits(bits: u64) -> Self { Self { bits } }
    pub const fn bits(self) -> u64 { self.bits }
    pub const fn contains(self, feature: Self) -> bool {
        (self.bits & feature.bits) != 0
    }
}

pub struct SbfSyscallInvocation {
    pub hash: u32,
    pub args: [u64; 5],
    pub runtime_features: SbfRuntimeFeatures,
}

pub enum SbfSyscallOutcomeV2 {
    Unregistered,
    Returned(u64),
    Fault(SbfErrorV2),
}

pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}

pub trait SbfEnvironmentV2 {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfErrorV2> {
        let _ = (hash, args);
        Err(SbfErrorV2::UnknownSyscall)
    }
    fn syscall_outcome(&mut self, hash: u32, args: [u64; 5])
        -> SbfSyscallOutcomeV2 {
        match self.syscall(hash, args) {
            Ok(value) => SbfSyscallOutcomeV2::Returned(value),
            Err(SbfErrorV2::UnknownSyscall) => SbfSyscallOutcomeV2::Unregistered,
            Err(error) => SbfSyscallOutcomeV2::Fault(error),
        }
    }
    // Some(SbfRuntimeFeatures::from_bits(0)) is an explicit empty snapshot.
    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> { None }
    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

pub fn neverd_sbf_program<E: SbfEnvironment>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfError> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated program body")
}
pub fn neverd_sbf_program_v2<E: SbfEnvironmentV2>(
    env: &mut E, input: u64, instruction_data: u64,
) -> Result<u64, SbfErrorV2> {
    let _ = (env, input, instruction_data);
    unimplemented!("generated v2 program body")
}
```

The original entrypoint and `SbfEnvironment` remain the v1 Result-based
contract. The separate `_v2` entrypoint uses `SbfEnvironmentV2`: its
`runtime_features` method returns an optional explicit snapshot (`Some` with
zero bits means an explicitly empty snapshot), and `syscall_with_features`
receives that snapshot with the hash and five arguments. `syscall_outcome` is
the compatibility bridge for Result-valued syscall hosts. `SbfErrorV2` is
`#[non_exhaustive]`; callers should use a wildcard match for future variants.
The generated entry points use fixed-size safe arrays for registers and call
frames. Representative output is compiled with `rustc --edition=2021
-D warnings` in the test suite.

## C API

Existing session operations work unchanged after loading SBF: synchronized
recovered-function lists, disassembly, Low/Med/High/LLVM dumps, CFG/call graph
JSON, sections, symbols, relocations, strings, and headers. Select Rust
explicitly with the appended output-language enum value; existing enum values
remain ABI-stable.

```c
neverd_session_t session = neverd_session_create();
neverd_sbf_set_strict(session, 1);
neverd_sbf_set_version(session, "auto");
/* Which runtime the answer is about. Defaults describe mainnet-beta as it
   stands, under loader-v3, for a program that is already deployed. */
neverd_sbf_set_cluster(session, "devnet");
neverd_sbf_set_slot(session, 474768000);
neverd_sbf_set_loader(session, "loader-v3");
neverd_sbf_set_purpose(session, "deployment");
/* Optional: name Anchor handlers from the program's own IDL. */
neverd_sbf_set_idl(session, idl_json);

const char *rust = neverd_decompile_all_ex(
    session, "program.so", NEVERD_OUTPUT_RUST, 0, 0);
/* consume rust, then: */
neverd_free_string(rust);
neverd_session_destroy(session);
```

## Verification and limitations

The reproducible evidence snapshot was audited on 2026-08-24 and is pinned by
`SBFUpstreamSources.def` to Anza `sbpf`
`2510663bb8d894e8e3094be351e4bb4b604f1f84`, Agave
`ef210d67f2fabeee1730498188fa78854260c679`, and the Solana SDK
`122f32e571ce39face4beffaccea733e37c207fd`; the external loader corpus is
Firedancer test-vectors
`68bb4af40235562e8852fa23d5727e49c2a0b862`.

| Evidence | Enforced contract |
|----------|-------------------|
| Official ELF manifest | All 23 pinned `sbpf/tests/elfs` artifacts have explicit load/execution outcomes and pass 23/23, covering legacy and strict layouts, relocations, calls, function pointers, data, and reject cases |
| Validator-compatible loader corpus | `NeverDSBFAgaveConformanceTests` authenticates the pinned Git tree and matches all 1,955 `sol_compat_elf_loader_v1` fixtures: 1,399 accepts and 556 rejects, plus `entry_pc`, `text_off`, `text_cnt`, `rodata_hash`, and `calldests_hash` for every accepted ELF |
| Independent official-process oracle | `NeverDSBFExternalOracleTests` drives the separately built pinned Anza verifier through the typed `SBFOfficialOracleProtocol.def` boundary and matches 1,411 opcode/verification-boundary probes across v0-v4 |
| Strict ELF differential | The separate `41-case strict ELF differential` sends deterministic strict-v3 header, range, overlap, entrypoint, and compatibility fixtures through one official `verify-elf-batch` process; these cases are not part of the 1,411 opcode/verifier total |
| Official execution differential | `SBFOfficialExecutionConstants.def` pins 508 active `(Version,Opcode)` cases plus 58 boundary cases: 566 exact official-process comparisons of outcome, canonical fault, result, instruction count, target hits, final input bytes, and syscall trace; these cases are separate from the 1,411 verifier probes and 41 strict-ELF cases |
| Hostile input corpus | `SBFOfficialELFMutations.def` and the malformed corpus cover overflow, overlap, truncation, inconsistent headers, optional-debug corruption, bad relocations, invalid registers, LDDW continuations, branches, and immediates; the table is the contract, so this document deliberately does not freeze a changing aggregate count |
| Typed verifier/fault model | `SBFOfficialVerifierCases.def` supplies upstream boundary cases, while `SBFValidationRules.def` and the raw-byte interpreter keep verifier, fault, and lowering decisions independently observable |
| Differential backends | LLVM ORC, generated C11, and safe stable Rust compare return/fault, writable memory, and syscall traces with the raw oracle; LLVM modules also pass `llvm::verifyModule` |
| CFG and scale invariants | `SBFEdgeKinds.def` distinguishes call and intraprocedural edges; per-function ownership, shared tails, multi-latch loops, gap-aware mapping, and dependency worklists have focused regressions including the 10,000-scale fixtures |
| Source ABI and Solana recovery | `SBFSourceStatuses.def` and `SBFArgumentRegisters.def` type the generated-source contract; Base58, Anchor, account layout, CPI, syscall windows, and recovered-fact silence are independently checked |
| Chain activation | `SBF_RUNTIME_VERSION` and runtime feature rows carry version, cluster, activation account, and slot. An `RPC activation audit` can compare those rows with a live node without making ordinary analysis network-dependent |

Linux Release CI reads the exact revisions and Rust toolchain with
`--print-pinned-revision`, `--print-test-vectors-revision`, and
`--print-toolchain`; it builds the detached official process and authenticates
a sparse corpus checkout before exporting `NEVERD_SBPF_ORACLE` and
`NEVERD_AGAVE_CONFORMANCE_ROOT`. Both external gates are therefore mandatory
there rather than silently skipped. A normal local build still discovers those
tests but may skip them when the explicit oracle/corpus environment variables
are absent. The loader gate intentionally does not run the instruction
verifier: Agave's loader corpus contains accepted ELFs that a later verifier
rejects, and conflating those stages would manufacture false loader mismatches.

The backend execution contract exposes `r0` as the return value, plus fault
status, VM memory effects, and syscall calls/results. Other final registers are
an internal implementation detail and are not claimed as an external ABI.

To refresh the evidence, update the full revisions in
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def`, and
`SBFUpstreamSources.def`; review upstream loader/verifier/config and Agave
syscall registration changes; then run:

```bash
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
  cmake --build build --target check-neverd-sbf
```

### Comparison-tool audit

The comparison tools are useful for presentation, execution, proof, and
recovery ideas, but are not interchangeable semantic oracles. The 2026-08-24
audit found:

- Blueshift `sbpf` at `704e40f7aa82446555b19d9ffbc0a6e18a35480f`
  is an active assembly scaffold, disassembler, and debugger;
  it is a useful UX/ISA cross-check, not a general source decompiler with this
  official-process and multi-backend differential evidence surface.
- `ezBPF` at `88829078a6d7682a2baed0d696d500401c263750` identifies itself as
  deprecated and points users to Blueshift. Its single byte-to-enum opcode map
  makes it a useful archived predecessor, not a version-aware decoder for
  moved-memory encodings, JMP32, and the rest of the current v0-v4 matrix.
- `qedsvm` at `99bd5ede85374adc7fc5c835c2432ecf4e123fd1`
  ties selected `.so` bytecode paths to machine-checked Lean theorems,
  a stronger path-proof capability than NeverD claims. Its current ELF loader
  explicitly accepts only V0 and its verification scope is selected paths, so
  it complements rather than replaces whole-program v0-v4 decompilation.
- `leanprover-solanalib` at
  `6c115ef1ef6a0cde8dbd6fd875b7dc87d60939ec` provides valuable formal
  decoder/interpreter/verifier
  material using its own two-version semantic model; it is a proof reference,
  not the current Agave/sbpf ELF and runtime-activation oracle.

- `sol-azy` at `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a` has no
  committed `Cargo.lock`. After pinning its broken transitive dependency only
  in an isolated temporary copy, it decoded the official legacy
  `relative_call_sbpfv0.so`; its `sbpf` v0.14.2 loader panicked on the current
  strict `relative_call.so`, and the generated legacy CFG retained an undefined
  node. NeverD keeps this tool as an advisory display baseline.
- `solana-data-reverser` at
  `bf90923adec984a61ca0437e9d341360ac1b11ee` analyzes account-data bytes and
  RPC metadata, not executable SBF semantics.
- The Ghidra-oriented `SolDragon` integration at
  `002b98677a5e595a773af6607b77210f5ea71db7` describes its stack-frame,
  VM-memory-map, syscall-name/signature, and analysis work as WIP.
- `bn-ebpf-solana` at `c3fe0de45d37eb68dcb08f2498c6e1f986056572`
  provides Binary Ninja UI/LLIL integration and SDK types, but requires Binary
  Ninja 5 plus plugin dependencies and was therefore not a headless oracle.
- `r2ghidra-solana` at
  `eca0b8e2d307e00991e289b8f9b0f45743819f1b` provides strong Ghidra C-like
  UX, `C-like-pdg`, and account/Anchor/string/syscall views. Its pinned CI
  passed, but its Solana-specific testsuite is commented out and the smoke test
  decompiles `/bin/ls`. In a direct reproduction it emitted reasonable C for
  official V0 `relative_call_sbpfv0.so`, while current official V3
  `relative_call.so` failed in `pdg`.
- `radare2-solana` at
  `292d845681be377cadc9959a74c2cadeb6e7f412` applies V2-only SIMD-0173/0174
  semantics to `>= V2`; pinned official `program.rs` restricts them to V2.
  It also has no V4 CPU profile.
- `SBPF-3-1` at `0e602c93007faa96bccb8e1e12040954ff108b6f`
  has two simple Cargo tests and no CI. Version detection still returns
  `None`/V0, its decoder matches the opcode high nibble, and its jump recovery
  uses `imm` instead of `off`; official V0 and V3 relative-call ELFs therefore
  produced the same incorrect pseudocode in the direct reproduction.

`SBFComparisonTools.def` is the single authority for comparison display names
and full revisions. The final bounded public sweep additionally found:

- `blastrock/Solana-eBPF-for-Ghidra` at
  `c3ad719004726fe924dbed901eca2744ad82c85d` offers real Ghidra P-code UX, but
  one unversioned SLEIGH model fixes CALLX to `dst`, mixes legacy/current
  opcodes, has no real tests or CI, and its default source lacks a referenced
  relocation-constant class.
- `SolEmu-Ghidra` at `6520af2ff104d5adbec24632ba3afa3bef0da529`
  inherits that byte-identical decoder and adds useful emulator UI around
  explicitly simulated or placeholder CPI, cryptographic, and ZK behavior;
  it likewise has no real tests or CI. `Ghidra_sBPF` at
  `907bd4476432ca83bb2352686ad1ccafdb38504c` offers manually selected v1-v3
  languages, but cumulatively includes V2-only encodings in V3, has no V0/V4
  auto-selection, and has no tests or CI.
- `solana-ebpf-ida-processor` at
  `aacd215907266190ed9c6c1b408ca9309f92ecdd` is a useful IDA disassembler and
  relocation UI, not a source lifter; its mixed opcode map always reads CALLX
  from `imm` and has no tests or CI. `solana-bpf-reverse` at
  `39479a3bddb8cb866ee499266a76a1b54069b222` emits heuristic reports and Rust
  TODO scaffolding from hard-coded layout guesses; its run had 9 passing, 2
  failing, and 1 skipped test, with no CI.
- `solens` at `22defa1c8f4118dacd42f5c291f1ac31609fc0e5` is a V2-only
  terminal disassembler with zero tests and no CI. `sbpf-decompiler` at
  `37b8bc0edc7ce347abee466f5f974e900c1948df` currently has a three-line
  `Hello, world!` implementation, zero tests, and no CI.
- `sbpf-eye` at `5277a52aeb58e50b6ff8f9020414334765369b49` is an explicitly
  lightweight WIP instruction/CFG TUI: 3 tests pass, but it has no semantic IR,
  source emitter, or CI. `svm_bytecode_analyzer` at
  `12aa236db8964e6be661e38131c2dc81588cf19c` is a disassembler/CFG analyzer,
  not a lifter; its register/offset byte decoding is incorrect and its test run
  had 17 passes and 1 failure, with no CI.
- `giraffexiu/Solana-eBPF-for-Ghidra` at
  `81c1e3c2b9ba35091e4a2d8bb6eb23fd59339f07` is a one-commit snapshot of the
  same Ghidra lineage, with no added version semantics, tests, or CI. `CertSBF`
  at `bb93a97cf0c64d119d08ec851e8e820315beb59e` is valuable Isabelle/HOL
  formalization of older rBPF semantics, not a current whole-program V0-V4
  source decompiler.

Official `sbpf` and Agave behavior remains the semantic authority.

On this bounded, reproducible 2026-08-24 audit, NeverD has the strongest
evidence we found among audited public general-purpose SBF decompilers. That is
a comparative engineering claim, not an absolute or permanent “world first”
claim; a newer tool or upstream revision can change it.

The final 2026-08-24 live `RPC activation audit` matched all 38 feature
accounts and 89 activation rows: finalized mainnet slot 441305159, testnet slot
433055669, and devnet slot 487238699. A system-owned empty pending account,
`VirtualAddressSpaceAdjustments` on mainnet, is correctly not treated as
activated. Ordinary analysis remains deterministic and offline, and the
documentation does not hard-code RPC URLs.

Intentional limits:

- SBF binary rewriting and object-code roundtrip are rejected explicitly.
- Anchor IDL/type recovery and live Solana RPC/account fetching are not part of
  the loader. They can be layered on the recovered addresses and call metadata.
- Generated source exposes syscalls and VM memory through a host contract; it
  is not a standalone Solana runtime.
- Relaxed mode is for inspection. Invalid instructions remain explicit and are
  never silently assigned guessed semantics.
