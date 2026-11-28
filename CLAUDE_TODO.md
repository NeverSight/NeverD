# CLAUDE_TODO

Open work left by the jump-table and CI campaign, as of 2026-11-28 (dev at
`1bad7c4d9e`). Items are ordered by what to do next.

## 1. Next action: CI on the current dev head

- CI run `46127034325` was dispatched on `9132922fa5` and was running when
  this was written; its repository checks passed. It is the first run to
  cover:
  - `4f54948cab`: `ELFDynamicRelocations.def` no longer forwards `__VA_ARGS__`
    into the three-field `NEVERD_ELF_SYMBOL_VALUE`; MSVC's traditional
    preprocessor passed it on as one argument (C4003 + C2059).
  - `f63f4f96f2` and `aa4e382e91`: dispatch by value through own-label relative
    tables (section 4).
  - `b8180a4371`: Win64 keeps `%rsi` and `%rdi` across calls for own-label table
    reads and the kept-register filter.
  - `ccc4482612`: a table jump whose every label reads a kept register keeps its
    whole table (gimli `FileEntry::clone`).
  - `bdb0414213`: `OwnInteriorCall.IndirectTailMustRestoreTheParentsStack`
    expects the x86-64 branch that `d59d652428` keeps when the frame is live.
- The run before it, `46089267845` on `e0f1a7d18a`, was the first whose Linux
  job finished the test profile within 6 h: 11 of 56149 failed. Besides the
  known `XCPT4` x3 and `a64cgo0` x5 (#2705 has since fixed the latter) they were the
  `OwnInteriorCall` case above and two contributor tests that dev had already
  fixed (`NeverDSemanticCAPI.LegacySimplificationRemainsMBAOnly` by #2725,
  `LoadDebugInfoTest.CheckedInNoUUIDDSYMDoesNotAuthenticateSemanticCapabilities`
  by #2732). Its Windows job failed only in `ELFDynamicRelocations.def`, and
  its macOS job ran out of runner disk.
- The run between them, `46121670615` on `05aa3730b7`, stopped in the
  repository checks: #2734, #2737 and #2738 had not claimed the data-pointer
  scan surfaces in `docs/capabilities.json`, which #2740 then did, and every
  build and test job was skipped.
- When `46127034325` ends, dispatch once more on the dev head, which adds
  `184d0b09d9` and `1bad7c4d9e`:
  `git push -f origin <head>:refs/heads/ci/return-contracts`, then
  `gh workflow run ci.yml --repo NeverSight/NeverD --ref ci/return-contracts -f use_prebuilt_llvm=true`.
  Run one CI at a time and do not cancel a running one.
- The Lift and Semantic suites do not run the separate unittest binaries.
  Before pushing a change to a pass, also build and run every binary whose
  tests exercise it; `bdb0414213` fixed a case missed that way.
- Find a Windows build error with
  `gh api repos/NeverSight/NeverD/actions/jobs/<job>/logs | grep -E 'FAILED:|error C[0-9]+'`.
  The build runs with `-k 20`, so the step fails only at its end, about 2 h 40 min
  in.
- Never forward `__VA_ARGS__` into another function-like macro that takes more
  than one parameter, and never use `__builtin_*_overflow`: MSVC rejects both.

## 2. Jump tables: unresolved dispatches left

After `1bad7c4d9e` the coverage set (545 items) has 276 unresolved indirect jumps
(`indirect targets not proved` outside a dispatch's default arm): 260
AArch64 and 16 x86-64 Linux Rust. The census (9 binaries) has 3, all in
pcre2.

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
- A disassembly survey of the 260 finds a compare-and-branch guard before
  the dispatch for at most about 40 (8 with the base hoisted out of a loop);
  the rest are unguarded enum dispatches (Rust `match`, MSVC ARM64 switches),
  so guarded-only tables would leave most of them unresolved.
- A bound for unguarded tables with almost no dead edges: code names each
  table's start (`adrp+add`), so the next address the code names in the same
  section ends the table, as `dispatchUnprovedBranchesByValue` already does
  for exact-field tables (`Anchors`). Alignment padding before the next
  object reads as zero, which names the anchor label itself. It needs the
  `adrp+add` targets of the whole image; no loader record lists them for a
  position-independent AArch64 image.

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

### 2.3 Absolute 8-byte tables

- The two Go 1.20/1.21 linux-amd64 jumps the coverage set had were no
  dispatch: Go indexes its absolute table with a constant after inlining
  (`mov $5, %ecx; lea table(%rip), %rdx; jmp *(%rdx,%rcx,8)`). `1bad7c4d9e` resolves
  them through exact branch recovery, which now reads the stored address in
  an image that runs at its link address (`readImmutableBranchScalar`).
- No unguarded absolute-table dispatch is left in the corpus. One would need
  the own-label machinery for 8-byte absolute slots (selector: the jump
  value; case keys: the labels' addresses), a non-PIE fixture, and a
  kept-register set that follows Go's ABIInternal instead of System V: Go
  passes integer arguments in RAX, RBX, RCX, RDI, RSI and R8-R11 and keeps
  `g` in R14, so the System V list would drop real Go labels that read
  `%rbx` or `%r14`.

### 2.4 Observation: a case that saves a kept register itself

- A shrink-wrapped prologue inside one case (`push %rbx` after the dispatch)
  reads a kept register before writing it, so unless every label does the
  same, that case loses its edge and traps. A temporary probe that reported
  every dropped edge found none in the corpus, census or coverage set: the
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
  import-binding rule). The `a64cgo0` round trips pass since #2705.

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
- Exact branch recovery (`CFGBuilder::recoverScalarConstantBranches`) reads
  immutable words through `readImmutableBranchScalar`: in an image that runs
  at its link address it also returns a stored address, since the walk only
  follows values to code. The immutable readers treat
  `BinaryImage::IndirectFunctionSlots` as loader fixups (`184d0b09d9`).
- The selector is the slot's `LOAD` (`OwnLabelTableReads`); case keys are
  `(int32)(label - base)`. That keeps the comparison correct in the recompiled
  program, where the table's address differs. The LLVM `emitValueDispatch`
  takes the selector from the switch selector plan when it is not the jump's
  own value; HighC already did.
- Tests: `JTE_X86_64.AnUnboundedOwnLabelTableDispatchesByValue`
  (`unittests/lift/x86_64/test_jumptable_own_label_dispatch.s`, including
  `jt_own_join`), `JTE_X86_64.AConstantIndexIntoAFixedImageTableBranchesToItsLabel`
  (`test_jumptable_fixed_constant_index.s`, linked as a static executable),
  `ImmutableScalar.IndirectFunctionSlotsHoldWhatTheResolverReturns`,
  `ImmutableScalar.AFixedImageKeepsTheAddressesItStoresForBranches`,
  `JTE_X86_64.AWin64OwnLabelDispatchKeepsRsiAndRdi`
  (`test_jumptable_own_label_win64.s`, a COFF object, so the test hands the
  builder the function entries the way the lift pipeline does), and the round
  trips `x64swx_enumbyte{0,3,5}` and `x64swx_enumload` in
  `NeverDSwitchXformTests`.
