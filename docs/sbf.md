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
`SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, `SBFSyscallMemory.def`, `SBFCPIABI.def`,
`SBFProgramInstructions.def`, `SBFKnownAddresses.def`,
`SBFAnchorNamespaces.def`, `SBFAnchorNames.def`, `SBFAccountLayout.def`,
`SBFLints.def`, and `SBFUpstreamSources.def`; ordinary one-use diagnostics and
LLVM block names remain local, matching LLVM's own `.def` policy.

`SBFProtocolLimits.def` records the historical 65,536-instruction value and
the current 10 MiB account-data ceiling; NeverD derives its conservative
decode bound from the latter.

After relocation, one immutable, VM-addressed `ProgramImage` is the semantic
source of truth. The decoder, interpreter, string recovery, LLVM backend, and
C/Rust backends all consume that same image; there is no independent rodata or
text copy that can drift from loader semantics.

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
what NeverD reports, each with the runtime identifier, the account whose
existence turns it on, and the slot each cluster activated it at. A gate with no
row for a cluster has not been activated there. `simd-0321` is on for every
cluster; `simd-0449` and the SHA-512 syscall are on for testnet and devnet and
off for mainnet, which is exactly why a program that works on devnet fails on
mainnet.

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
`sol_alloc_free_` needs no gate at all: the runtime keeps honouring it and
refuses to accept a new program that calls it, which is a difference between
the two registries and nothing else.

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

A call to a function this analysis has not described is assumed to write
anything it can reach. A callee runs in a frame of its own, so a call whose
argument registers are all proven not to address scratch leaves the model
intact; anything else discards it. `sol_invoke_signed_rust` and
`sol_invoke_signed_c` write account data rather than caller memory, so two
invocations assembled in one block are both readable.

The model is a forward must-analysis over the intra-function CFG: a byte
survives into a block only when every path that reaches it wrote the same value.
Call edges are not followed, because a callee inherits nothing from its caller's
frame. Programs with more than `kMaxScratchFlowBlocks` blocks keep per-block
recovery and lose only the facts that cross a block boundary.

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
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t address, uint32_t width, uint64_t *value);
  int (*store)(void *, uint64_t address, uint32_t width, uint64_t value);
  int (*syscall)(void *, uint32_t hash,
                 uint64_t r1, uint64_t r2, uint64_t r3,
                 uint64_t r4, uint64_t r5, uint64_t *result);
} neverd_sbf_environment;
```

`width` is in bits. A nonzero host return becomes an explicit generated SBF
status. Registers, return PCs, callee-saved r6-r9, frame pointers, VM addresses,
division faults, wide PQR operations, and wrapping shifts are represented in
the generated source. Only helpers actually used by the program are emitted,
so `clang -Wall -Wextra -Werror` accepts minimal output.

## Generated Rust host contract

Rust output is stable, safe Rust and uses a trait rather than raw pointers:

```rust
pub trait SbfEnvironment {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError>;
    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError>;
    fn syscall(&mut self, hash: u32, args: [u64; 5])
        -> Result<u64, SbfError>;
}
```

The generated entry point is generic over this trait and uses fixed-size safe
arrays for registers and call frames. Representative output is compiled with
`rustc --edition=2021 -D warnings` in the test suite.

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

The conformance baseline was audited on 2026-08-10 against Anza `sbpf`
`71425d0de59e0bff048c6be8f4a8a9bc655916e2` and Agave master
`cae40aa610fdbdb313209bc1eec737079eb59688`.

| Evidence | Enforced contract |
|----------|-------------------|
| Official ELF manifest | All 20 `sbpf/tests/elfs` artifacts have explicit load/execution outcomes and currently pass 20/20, including legacy relocated data, strict headers, relative calls, and function pointers |
| Exhaustive ISA matrix | Every one of 256 byte encodings is checked for each v0-v4 version (1,280 version/encoding cells), plus verifier boundary cases |
| Hostile input corpus | Overflowed ELF tables/segments, overlaps, malformed optional metadata, invalid registers, LDDW continuations, branches, and immediate domains are rejected or quarantined at the intended boundary |
| Raw-byte oracle | Executes verified instruction bytes without consuming MedIR, so MedIR construction/corruption and backend-lowering defects cannot automatically agree; explicit upstream outcomes and semantic unit tests independently constrain the shared typed semantic model |
| LLVM ORC differential | Compares return/fault, writable memory, and syscall trace across versioned arithmetic, calls/CALLX, memory, syscalls, and runtime faults |
| C/Rust execution differential | Compiles generated C11 with `-Werror` and stable Rust with `-D warnings`, then compares the same observable state, including an official relocated-data ELF |
| Solana recovery | Base58 round-trips the whole byte domain, every known address decodes to 32 bytes and re-encodes to its recorded spelling, Anchor discriminators match the published reference values, the account layout tiles without a gap, and end-to-end recovery is checked to stay silent when nothing is proven |
| Integrated SBF aggregate | `check-neverd-sbf` discovers and passes 145/145 cases across 14 binaries, including four public C API integration cases |
| ASan + UBSan | 141/141 core cases across 13 binaries pass with fail-fast sanitizer settings; the public integration binary is linked and run in the integrated build because the prebuilt LLVM package omits the NeverD fork-only header it requires |

The backend execution contract exposes `r0` as the return value, plus fault
status, VM memory effects, and syscall calls/results. Other final registers are
an internal implementation detail and are not claimed as an external ABI.

To refresh the evidence, update the full revisions in
`SBFUpstreamManifest.def`, `SBFUpstreamOpcodes.def`, and
`SBFUpstreamSources.def`; review upstream loader/verifier/config and Agave
syscall registration changes; then run:

```bash
NEVERD_SBPF_ROOT=$PWD/local_docs/sbpf \
  cmake --build build --target check-neverd-sbf
```

### Comparison-tool audit

The comparison tools are useful for presentation and recovery ideas, but are
not interchangeable semantic oracles. The 2026-08-10 local audit found:

- `sol-azy` at `362327a798e5dad6e12aa9abf3ed9ed52c17ef6a` has no
  committed `Cargo.lock`. After pinning its broken transitive dependency only
  in an isolated temporary copy, it decoded the official legacy
  `relative_call_sbpfv0.so`; its `sbpf` v0.14.2 loader panicked on the current
  strict `relative_call.so`, and the generated legacy CFG retained an undefined
  node. NeverD keeps this tool as an advisory display baseline.
- `solana-data-reverser` at
  `bf90923adec984a61ca0437e9d341360ac1b11ee` analyzes account-data bytes and
  RPC metadata, not executable SBF semantics.
- `SolDragon` at `002b98677a5e595a773af6607b77210f5ea71db7` describes its
  stack-frame, VM-memory-map, syscall-name/signature, and analysis work as WIP.
- `bn-ebpf-solana` at `c3fe0de45d37eb68dcb08f2498c6e1f986056572`
  provides Binary Ninja UI/LLIL integration and SDK types, but requires Binary
  Ninja 5 plus plugin dependencies and was therefore not a headless oracle.

Official `sbpf` and Agave behavior remains the semantic authority.

Intentional limits:

- SBF binary rewriting and object-code roundtrip are rejected explicitly.
- Anchor IDL/type recovery and live Solana RPC/account fetching are not part of
  the loader. They can be layered on the recovered addresses and call metadata.
- Generated source exposes syscalls and VM memory through a host contract; it
  is not a standalone Solana runtime.
- Relaxed mode is for inspection. Invalid instructions remain explicit and are
  never silently assigned guessed semantics.
