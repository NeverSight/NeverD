**Languages**: [English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Français](README.fr.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Italiano](README.it.md) | [Русский](README.ru.md) | [العربية](README.ar.md)

[← Documentation Index](../README.md)

# NeverD Roadmap

This document outlines major planned directions for NeverD beyond today’s native PE / ELF / Mach-O pipeline. The same principles apply everywhere: **1:1 instruction-level lifting**, **strict fail-loud semantics** (unsupported ops error instead of silent gaps), and a shared **four-stage IR** feeding lift / decompile / patch.

---

## 1. Native format completeness

Finish container-level support for targets the loaders already partially recognize, so the support matrix matches what users can actually lift end-to-end.

| Item | Notes |
|------|--------|
| PE AArch64 | Windows ARM64 images: unwind/`.pdata`, trampolines, rewrite roundtrip |
| PE ARM32 (Thumb-2) | Windows on ARM is Thumb-only; decode/emit must honor that mode |
| Mach-O i386 | Apply common clang relocations; thin objects first |

**Status:** Complete. Format-level coverage is locked by
[`COFFARMFormatTests.cpp`](../../unittests/lift/format/COFFARMFormatTests.cpp),
[`MachOI386RelocationTests.cpp`](../../unittests/lift/format/MachOI386RelocationTests.cpp),
and the PE/Mach-O cases in
[`PatchFormatTests.cpp`](../../unittests/lift/format/PatchFormatTests.cpp). Mach-O i386
uses PIC and no-PIC thin objects because modern macOS hosts cannot link
historical i386 executables.

### Design principles

- Do not mark a format×arch cell supported until format-level tests pass (load → lift → decompile / patch)
- Keep existing ELF / PE x86 / Mach-O arm64+x64 behavior unchanged
- Prefer one image-level instruction mode (e.g. Thumb vs ARM) over scattered heuristics

---

## 2. EVM bytecode decompilation

Extend NeverD from native ISAs to **Ethereum Virtual Machine (EVM)** contract bytecode — lift EVM opcodes into the same IR stack and emit C, Solidity-oriented source, and LLVM IR for audit and analysis.

### Goals

- **EVM loader** — accept raw runtime bytecode and common artifact wrappers (e.g. deployed code, creation vs runtime split)
- **Opcode lifter** — hand-written 1:1 semantics for the EVM instruction set; unknown/new opcodes fail loud under strict mode
- **Stack & memory model** — recover EVM stack machine state into MedIR variables / memory ops
- **Control-flow recovery** — JUMP / JUMPI → CFG; structured HighIR where possible
- **Storage & calldata** — model `SLOAD`/`SSTORE`, calldata, returndata, and common ABI call patterns
- **Decompile outputs** — compilable C23 and Solidity-oriented state machines,
  with explicit host-effect contracts, plus verified LLVM IR
- **CLI / C API** — `neverd decompile` / session APIs work on EVM inputs the same way as native binaries

**Status:** Complete for legacy EVM bytecode from Frontier through Fusaka. The
implementation covers all 150 assigned legacy opcodes, raw/hex/artifact inputs,
creation-to-runtime extraction, CFG and stack-SSA recovery, strict and relaxed
analysis, C23/LLVM/Solidity backends, and CLI/C API integration. The focused EVM
suite includes interpreter-to-generated-C, LLVM, and deployed Solidity
differential execution, plus an independent comparison against Anvil's native
EVM. Four Amsterdam opcodes are additionally available behind an explicit
development-fork target, with official/upstream vectors and metadata-drift
auditing while `latest` remains finalized Fusaka. See
[EVM decompilation](../evm.md) for the host
ABI and the intentionally explicit limits around dynamic jumps, external host
effects, heuristic high-level naming, and EOF bytecode.

### Why EVM in NeverD?

- Auditors already need faithful recovery of on-chain logic; approximate decompilers hide semantics
- Reusing Low → Med → High → LLVM keeps one engine for native and contract analysis
- Fail-loud lifting matches NeverD’s native contract: no silent “unsupported opcode → skip”

---

## 3. Solana eBPF (SBF) decompilation

Support **Solana’s eBPF / SBF** on-chain programs — lift SBF machine code into NeverD IR and decompile with the same strict semantics.

### Goals

- **SBF / sbpf loader** — load Solana program ELF objects (and related packaging as needed)
- **eBPF/SBF lifter** — 1:1 hand-written semantics for the Solana BPF ISA subset; strict errors on gaps
- **Account & CPI awareness** — recover common Solana runtime patterns (account infos, syscalls, CPI) where they appear as calls/intrinsics
- **CFG & structured output** — same pipeline as native: LowIR → MedIR → HighIR / LLVM → C
- **CLI / C API** — uniform session load / analyze / decompile entry points

**Status:** Complete for the current Anza `sbpf` v0-v4 contracts. The
implementation supports legacy section/relocation ELFs and strict
program-header-only ELFs, a complete versioned instruction database, strict
verification, staged Low/Med/High IR, syscall/CPI/account observations,
verified LLVM, portable C11, safe stable Rust, CLI/C API integration, and an
independent bounded raw-bytecode semantic oracle. v4 is tracked from upstream;
whether it can be deployed or executed on a particular cluster still depends
on that cluster's feature activation. See [Solana SBF decompilation](../sbf.md).

### Why Solana eBPF in NeverD?

- On-chain SBF is a first-class audit target alongside EVM
- BPF-shaped ISAs fit NeverD’s existing CFG + SSA MedIR approach
- One C SDK for native + contract bytecode reduces tool fragmentation for security research

---

## 4. Memory-safety audit & hunt

Analyse a lifted binary for heap-lifetime defects (leak, double free, use after
free) and dangerous-copy overflows, reporting structured JSON with a concrete
witness for a proven overflow. The analysis runs on the format-neutral IR and
the shared identity view, so **PE, ELF, and Mach-O are co-equal targets**, and
it reuses the in-house symbolic execution and bitvector solver — no external
solver or container.

| Item | Notes |
|------|--------|
| `audit` track | Heap state machine over IR + escape summaries: leak, double free, use after free |
| `hunt` track | Sink catalog + argument prefilter + destination capacity + solver witness |
| Identity contract | Per-format sink resolution (PE IAT, ELF PLT, Mach-O dyld bind) and PDB / DWARF / MAP name sources |

**Status:** P0 complete for PE, ELF, and Mach-O. Verdict and identity coverage
is locked by
[`unittests/safety`](../../unittests/safety) (catalog, scanner, argument
prefilter, object model, hunt, audit) and an end-to-end
[`SafetyIntegrationTests.cpp`](../../unittests/safety/SafetyIntegrationTests.cpp)
that runs the mandatory PE/ELF/Mach-O × x86-64/AArch64 fixture matrix on every
host. See
[Memory-safety audit & hunt](../memory-safety.md). P1 widens to stack/global
overflow, uninitialised reads, and format strings.

---

## 5. Engine & product hardening (ongoing)

Cross-cutting work that unblocks the items above and improves today’s native engine.

| Area | Direction |
|------|-----------|
| Lifter coverage | Close remaining native opcode gaps without relaxing strict mode |
| Semantic tests | Expand Unicorn / roundtrip coverage as new ISAs land |
| Plugin ABI | Loaders or analysis passes for new formats as plugins where it fits |
| Docs / matrix | Update README support tables only after tests land |

---

## Timeline

Native format completeness, legacy EVM decompilation, and Solana SBF
decompilation are complete and regression-covered. No release dates are
committed. Progress will be tracked here.

| Feature | Status |
|---------|--------|
| Native format completeness (PE ARM*, Mach-O i386) | Complete — regression-covered |
| EVM bytecode decompilation | Complete — C, Solidity, and LLVM; regression-covered |
| Solana eBPF (SBF) decompilation | Complete — v0-v4, C, Rust, and LLVM; regression-covered |
| Memory-safety audit & hunt | Complete — P0 for PE, ELF, and Mach-O; regression-covered |
| Engine & product hardening | Ongoing |
