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
       ├─ verified LLVM IR
       ├─ portable C11
       └─ safe stable Rust
```

The implementation follows the current Anza `sbpf` VM rather than treating
Solana programs as generic Linux eBPF. Its version, opcode, syscall, relocation,
call-argument ABI, and protocol metadata live in the `.def` databases under
`include/neverd/sbf/`; loaders and backends consume generated typed tables
instead of duplicating encodings or spellings.

The closed tables include `SBFVersions.def`, `SBFOpcodes.def`,
`SBFRelocations.def`, `SBFArgumentRegisters.def`, `SBFProtocolLimits.def`,
`SBFSyscalls.def`, and
`SBFUpstreamSources.def`; ordinary one-use diagnostics and LLVM block names
remain local, matching LLVM's own `.def` policy.

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
not `version >= N` guesses. Strict mode is the default and rejects malformed
headers, ranges, alignments, unsupported writable legacy sections, invalid
continuations, bad registers, illegal frame-pointer writes, invalid branches,
and version-inactive opcodes with instruction slot and virtual address.

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

# Inspect every analysis stage.
neverd lift --dump-low program.so
neverd lift --dump-med program.so
neverd lift --dump-high program.so

# Verified LLVM IR.
neverd lift -o program.ll program.so

# Both source backends are first-class.
neverd decompile --language=c -o program.c program.so
neverd decompile --language=rust -o program.rs program.so

# Decode a fixture with an explicit VM contract, or retain malformed input for
# forensic inspection. Auto-detection and strict verification are preferred.
neverd lift --sbf-version=v2 program.so
neverd lift --sbf-relaxed --dump-low program.so
```

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
marked as account/input access. This layer deliberately does not invent Anchor
types or account layouts without an IDL.

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
| Integrated SBF aggregate | `check-neverd-sbf` discovers and passes 104/104 cases across 13 binaries, including three public C API integration cases |
| ASan + UBSan | 101/101 core cases across 12 binaries pass with fail-fast sanitizer settings; the public integration binary is linked and run in the integrated build because the prebuilt LLVM package omits the NeverD fork-only header it requires |

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
