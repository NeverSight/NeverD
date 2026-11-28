# CLAUDE_TODO

Open work left by the jump-table and CI campaign, as of 2026-11-28 (dev at
`ccc4482612`). Items are ordered by what to do next.

## 1. Next action: CI on the current dev head

- CI run `46089267845` (on `e0f1a7d18a`) was still running its Linux job when
  this was written. Its Windows job failed only in `ELFDynamicRelocations.def`
  (C2059, fixed by `4f54948cab`); its macOS job ran out of runner disk
  (section 3).
- When it ends, dispatch once more on the dev head:
  `git push -f origin <head>:refs/heads/ci/return-contracts`, then
  `gh workflow run ci.yml --repo NeverSight/NeverD --ref ci/return-contracts -f use_prebuilt_llvm=true`.
  Run one CI at a time and do not cancel a running one.
- That run is the first to cover on CI:
  - `4f54948cab`: `ELFDynamicRelocations.def` no longer forwards `__VA_ARGS__`
    into the three-field `NEVERD_ELF_SYMBOL_VALUE`; MSVC's traditional
    preprocessor passed it on as one argument (C4003 + C2059).
  - `f63f4f96f2` and `aa4e382e91`: dispatch by value through own-label relative
    tables (section 4).
  - `b8180a4371`: Win64 keeps `%rsi` and `%rdi` across calls for own-label table
    reads and the kept-register filter.
  - `ccc4482612`: a table jump whose every label reads a kept register keeps its
    whole table (gimli `FileEntry::clone`).
- Find a Windows build error with
  `gh api repos/NeverSight/NeverD/actions/jobs/<job>/logs | grep -E 'FAILED:|error C[0-9]+'`.
  The build runs with `-k 20`, so the step fails only at its end, about 2 h 40 min
  in.
- Never forward `__VA_ARGS__` into another function-like macro that takes more
  than one parameter, and never use `__builtin_*_overflow`: MSVC rejects both.

## 2. Jump tables: unresolved dispatches left

After `ccc4482612` the coverage set (545 items) has 278 unresolved indirect jumps
(`indirect targets not proved` outside a dispatch's default arm): 260
AArch64, 16 x86-64 Linux Rust, 2 Go amd64. The census (9 binaries) has 3, all
in pcre2.

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

### 2.2 Lost-validated tables (pcre2 0x61BDF, 0x61C0C, 0x61D07; Rust x2 per image)

- These jumps published a validated table and lost it in final revalidation,
  so `recoverOwnLabelTableBranches` skips them
  (`LostValidatedJumpTableBranches`). They are all 16 x86-64 Rust jumps left
  (0x16A27 and 0x16BA4 in `rust_eh_probe-x86_64-unknown-linux-gnu-abort-o2`,
  the same pair at other addresses in the other seven images).
- Letting them dispatch by value resolves all of pcre2 (16 dispatches, 0
  unresolved) but breaks
  `JTE_X86_64.SecondMachineBehindAnOpenEntryDispatchStaysUnreachable`, the
  TwoTable cascade test: an entry dispatch that can never close must not make
  the second machine reachable (the leading run reads into the adjacent
  table). It was reverted; this needs a decision, or a resolver fix for why
  the validation is lost.

### 2.3 Absolute 8-byte tables (Go 1.20/1.21 linux-amd64, 2 jumps)

- Non-PIE Go uses absolute slots (`jmp *table(,%rax,8)`). Support existed in a
  draft of `f63f4f96f2` and resolved both, but it was removed for lack of a
  test. It needs a round trip compiled with `-fno-pic`, or a linked non-PIE
  fixture, before it comes back. Selector: the jump value itself, with the
  labels' addresses as case keys; the existing value-dispatch slot mirror
  keeps relocated slots numeric.
- When it lands, the kept-register filter and `TableReadState` must follow
  Go's ABIInternal instead of System V: Go passes integer arguments in RAX,
  RBX, RCX, RDI, RSI and R8-R11 and keeps `g` in R14, so the System V list
  would drop real Go labels that read `%rbx` or `%r14`.

### 2.4 Observation: a case that saves a kept register itself

- A shrink-wrapped prologue inside one case (`push %rbx` after the dispatch)
  reads a kept register before writing it, so unless every label does the
  same, that case loses its edge and traps. A probe that reported every
  dropped edge (an env-gated `fprintf` in a `tools/shadow.py` copy, linked
  with `tools/qlib.py`) found none in the corpus, census or coverage set: the
  only register drops were gimli's, now kept. LLVM does not shrink-wrap for
  Windows CFI, and on System V it rarely moves a save below a table dispatch.

## 3. Decisions waiting on the maintainer

- AArch64 compact tables: guarded-only, or also unguarded ones with dead edges
  (2.1).
- Lost-validated tables: keep opaque, or dispatch by value (2.2).
- Undescribed callee arguments: an undescribed external callee gets no
  trailing argument registers that no path set (`936bbbb654`), so
  `ext_real(p, q)` in `krpass.c` renders as `ext_real()`. In newly decoded
  code, registers set on only some paths still become guarded arguments
  (`v_known ? v : trap`); sqlite `parse.o` gained 371 such markers when its
  parser was decoded. gimli `FileEntry::clone` now shows the same guard where
  it stores an unused payload from `%r15` and `%r11`.
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
  constant propagation of whole registers; a call keeps only what
  `TargetRegInfo::isCallPreserved` keeps under the image's `abiFormat()`, so
  Win64 also keeps `%rsi` and `%rdi`). It finds
  `target = base + sext(load32(base + 4 * i))` in a read-only, non-executable
  section.
- Labels are the leading slots that are linear-sweep instruction starts and
  `isOwnedInteriorTarget`. Rounds continue up to `kMaxOwnLabelTableRounds`;
  after each round every dispatch is re-read on the grown graph and withdrawn
  for good if its table read changed.
- `dispatchUnprovedBranchesByValue` keeps an edge only where every kept
  register the label's code reads before writing was written on every path
  to the jump. A table jump whose every label fails that test keeps its whole
  table, and the registers it read stop counting for the function's table
  jumps (`Own`).
- The selector is the slot's `LOAD` (`OwnLabelTableReads`); case keys are
  `(int32)(label - base)`. That keeps the comparison correct in the recompiled
  program, where the table's address differs. The LLVM `emitValueDispatch`
  takes the selector from the switch selector plan when it is not the jump's
  own value; HighC already did.
- Tests: `JTE_X86_64.AnUnboundedOwnLabelTableDispatchesByValue`
  (`unittests/lift/x86_64/test_jumptable_own_label_dispatch.s`, including
  `jt_own_join`), `JTE_X86_64.AWin64OwnLabelDispatchKeepsRsiAndRdi`
  (`test_jumptable_own_label_win64.s`, a COFF object, so the test hands the
  builder the function entries the way the lift pipeline does), and the round
  trips `x64swx_enumbyte{0,3,5}` and `x64swx_enumload` in
  `NeverDSwitchXformTests`.
