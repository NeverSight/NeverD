# CLAUDE_TODO

Open work left by the jump-table and CI campaign, as of 2026-11-28 (dev at
`e71c00e20e`). Items are ordered by what to do next.

## 1. Next action: CI on the current dev head

- CI run `46089267845` was dispatched on `e0f1a7d18a`, before the last three
  fixes landed. When it ends, dispatch once more on the dev head:
  `git push -f origin <head>:refs/heads/ci/return-contracts`, then
  `gh workflow run ci.yml --repo NeverSight/NeverD --ref ci/return-contracts -f use_prebuilt_llvm=true`.
  Run one CI at a time and do not cancel a running one.
- That run is the first to cover on CI:
  - `e0f1a7d18a`: the x86-64 tail-frame guard checks stack offsets with
    `llvm::AddOverflow`/`llvm::SubOverflow` (MSVC has no
    `__builtin_*_overflow`). Run `46089267845` already confirmed that this file
    now builds on Windows.
  - `4f54948cab`: `ELFDynamicRelocations.def` no longer forwards `__VA_ARGS__`
    into the three-field `NEVERD_ELF_SYMBOL_VALUE`; MSVC's traditional
    preprocessor passed it on as one argument (C4003 + C2059). This was the
    Windows build error in run `46089267845`.
  - `f63f4f96f2` and `aa4e382e91`: dispatch by value through own-label relative
    tables (section 2).
- Find a Windows build error with
  `gh api repos/NeverSight/NeverD/actions/jobs/<job>/logs | grep -E 'FAILED:|error C[0-9]+'`.
  The build runs with `-k 20`, so the step fails only at its end, about 2 h 40 min
  in.
- Never forward `__VA_ARGS__` into another function-like macro that takes more
  than one parameter, and never use `__builtin_*_overflow`: MSVC rejects both.

## 2. Jump tables: unresolved dispatches left

After `aa4e382e91` the coverage set (545 items) still has 286 unresolved
indirect jumps (`indirect targets not proved` outside a dispatch's default
arm): 260 AArch64, 24 x86-64 Linux Rust, 2 Go amd64. The census (9 binaries)
has 3, all in pcre2.

### 2.1 AArch64 compact tables (260 jumps)

- Shape (LLVM, mostly Rust `match` on an enum discriminant):
  `adrp+add` table base, `adr` anchor label, `ldrb`/`ldrh wN, [base, idx(, lsl #1)]`,
  `add xT, anchor, wN, lsl #2`, `br xT`. Some are guarded (`cmp wI, #K; b.hi`)
  with the base hoisted into a callee-saved register (miniz `decompress`
  0x34794 and 0x580C in the aarch64 Rust probes); most are unguarded.
- LowIR lifts the register-offset byte/halfword load as `INTRINSIC 0x5DD`
  (inputs: base, index, instruction encoding), not as `LOAD`, so
  `recoverOwnLabelTableBranches` (x86-64 only today) cannot read it yet.
- Open design question: every 4-byte-aligned address is an instruction start
  on AArch64, so the x86-64 linear-sweep check that ends an over-read table
  gives nothing. An unguarded byte table has no natural end, and reading past
  it adds dead edges into the function. Options: dispatch only guarded tables,
  capped by the guard bound (sound and precise), or accept dead edges for
  unguarded ones. A dispatch would compare the zero-extended slot with
  `(label - anchor) >> 2`.

### 2.2 Lost-validated tables (pcre2 0x61BDF, 0x61C0C, 0x61D07; Rust 0x16A27, 0x16BA4)

- These jumps published a validated table and lost it in final revalidation,
  so `recoverOwnLabelTableBranches` skips them
  (`LostValidatedJumpTableBranches`).
- Letting them dispatch by value resolves all of pcre2 (16 dispatches, 0
  unresolved) but breaks
  `JTE_X86_64.SecondMachineBehindAnOpenEntryDispatchStaysUnreachable`, the
  TwoTable cascade test: an entry dispatch that can never close must not make
  the second machine reachable (the leading run reads into the adjacent
  table). It was reverted; this needs a decision, or a resolver fix for why
  the validation is lost.

### 2.3 Kept-register filter drops a real dispatch (x86-64 Rust 0x3E8E4)

- gimli `FileEntry::clone` dispatches on two nested enums. One path copies the
  caller's `%r15` into an unused field, a real read of a callee-saved register
  before any write, so `dispatchUnprovedBranchesByValue` drops every edge of
  the first dispatch. Zero idioms are not the cause: `xor r, r` and `sub r, r`
  already lift to `COPY 0`.

### 2.4 Absolute 8-byte tables (Go 1.20/1.21 linux-amd64, 2 jumps)

- Non-PIE Go uses absolute slots (`jmp *table(,%rax,8)`). Support existed in a
  draft of `f63f4f96f2` and resolved both, but it was removed for lack of a
  test. It needs a round trip compiled with `-fno-pic`, or a linked non-PIE
  fixture, before it comes back. Selector: the jump value itself, with the
  labels' addresses as case keys; the existing value-dispatch slot mirror
  keeps relocated slots numeric.

### 2.5 Windows x64 callee-saved registers

- `getTargetRegInfo(Arch::X64).CalleeSaveRegs` is the System V list (no
  `%rsi`/`%rdi`). On PE x64 the kept-register filter therefore does not filter
  reads of `%rsi`/`%rdi`, and the hoisted-base propagation forgets them across
  calls (conservative). A per-ABI list would fix both.

## 3. Decisions waiting on the maintainer

- AArch64 compact tables: guarded-only, or also unguarded ones with dead edges
  (2.1).
- Lost-validated tables: keep opaque, or dispatch by value (2.2).
- Undescribed callee arguments: an undescribed external callee gets no
  trailing argument registers that no path set (`936bbbb654`), so
  `ext_real(p, q)` in `krpass.c` renders as `ext_real()`. In newly decoded
  code, registers set on only some paths still become guarded arguments
  (`v_known ? v : trap`); sqlite `parse.o` gained 371 such markers when its
  parser was decoded.
- CI infrastructure: Linux and Windows jobs hit GitHub's 6 h job cap before
  the main test profile finishes, and the macOS runner runs out of disk
  (`ld: write() failed, errno=28`) while linking the test binaries.
- Known failures kept as decided earlier: `XCPT4` x3 (documented
  import-binding rule) and `CGotoO0/A64CGotoRT.Verify/a64cgo0_loc4` and
  `a64cgo0_loc8` (AArch64 frame-table certificate; CI also shows
  `CGotoO0HighC/...a64cgo0_loc8`).

## 4. Reference: dispatch by value through own-label tables

- `CFGBuilder::recoverOwnLabelTableBranches`
  (`lib/ir/low/jumptable/JumpTableResolverInterior.cpp`) runs once after the
  resolver's fixed point; the resolver is not run again, because that would
  retry proofs whose evidence ran out and drop their incompleteness markers.
- `TableReadState` evaluates a jump's single-predecessor chain
  (`kMaxOwnLabelTableReadBlocks`), seeded by `blockEntryConstants` (forward
  constant propagation of whole registers; a call keeps only callee-saved
  registers). It finds `target = base + sext(load32(base + 4 * i))` in a
  read-only, non-executable section.
- Labels are the leading slots that are linear-sweep instruction starts and
  `isOwnedInteriorTarget`. Rounds continue up to `kMaxOwnLabelTableRounds`;
  after each round every dispatch is re-read on the grown graph and withdrawn
  for good if its table read changed.
- The selector is the slot's `LOAD` (`OwnLabelTableReads`); case keys are
  `(int32)(label - base)`. That keeps the comparison correct in the recompiled
  program, where the table's address differs. The LLVM `emitValueDispatch`
  takes the selector from the switch selector plan when it is not the jump's
  own value; HighC already did.
- Tests: `JTE_X86_64.AnUnboundedOwnLabelTableDispatchesByValue`
  (`unittests/lift/x86_64/test_jumptable_own_label_dispatch.s`) and the round
  trips `x64swx_enumbyte{0,3,5}` and `x64swx_enumload` in
  `NeverDSwitchXformTests`.
