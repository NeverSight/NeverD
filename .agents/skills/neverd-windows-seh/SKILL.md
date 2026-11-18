---
name: neverd-windows-seh
description: >
  Fix or review Windows structured exception handling (SEH) and Control Flow
  Guard (CFG) behavior in NeverD end to end: x64/ARM/AArch64 scope tables,
  x86 registration SEH, GS handler wrappers, `_local_unwind`, filter and
  termination handler funclets, `__try`/`__except`/`__finally` recovery in
  HighC and LLVM C, native lowering, and the COFF patch output gate. Use when
  a decompiled function prints wrong or missing SEH, carries a
  "neverd.seh: ..." or "neverd.not-decompiled" note, when the user pastes SEH
  output to fix, when a rewrite (patch) of a PE with exception tables or CFG
  is involved, or when the user runs /neverd-windows-seh.
---

# NeverD Windows SEH

Windows SEH crosses every layer of NeverD: the loader reads the tables, LowIR
and MedIR recover the handlers and their frames, the LLVM emitter lowers or
annotates them, two C writers print them, and the patch path must execute
them again. A fix that is right in one layer and silent in another is how
this area went wrong before. Read this before touching any of it.

Related skills: `neverd-x64-c-quality` owns how settled C looks (names,
locals, the printed `__try` forms); `neverd-semantic-invariant-debugging`
owns duplicated decisions in general; `neverd-performance` owns speed claims.

## Working rules

1. **Own the decision once.** Find the layer that decides (see
   [pipeline-map.md](references/pipeline-map.md)) and fix it there. HighC and
   LLVM C must agree; where HighC is right, it is the reference for LLVM C.
2. **Prove by running, not by reading.** A cause stated from code reading is a
   hypothesis. Twice a confident cause in this area was overturned by a
   one-line experiment. Say "read from the code, not shown by a run" until a
   run shows it.
3. **Execute what you emit.** Output that cannot be shown right by execution
   refuses by name (semantics-first). A native table or a rewritten image is
   not done until the image ran and its transcript equals the original's
   ([verification.md](references/verification.md)).
4. **Solve the whole request.** When the user asks to solve a list, every item
   ends as a landed fix with evidence, or as a decision only the user can
   make, stated with measured numbers. "By design" needs the reason and what
   is left. When the user then says to do all of them, do all of them.
5. **Push and keep going.** Push finished work to `dev` at once, dispatch CI
   on a `ci/` ref, and do not wait for it; compare its verdict by test name
   later. Rebuild the hand-over (real engine) build when the user tests in the
   workbench.
6. **Report what is true.** Per commit: what changed, the corpus numbers, which
   CI platforms gave a verdict and which did not, and any regression a commit
   carried until a later one fixed it.

Decisions that belong to the user: accepting a new native output shape,
adding a new printed output form, a wide rule that changes many functions'
text, and refusing versus binding an unresolved runtime reference. Bring the
numbers; do not decide them silently either way.

## References

- [semantics.md](references/semantics.md): the Windows facts that decide
  correctness (scope-table order, GS wrappers, local unwind, handler ABI,
  x86 registration SEH, ARM/AArch64 callbacks, CFG dispatch).
- [pipeline-map.md](references/pipeline-map.md): where each decision lives,
  the metadata that carries it, and the tests next to it.
- [verification.md](references/verification.md): sweeps, runtime proof,
  mutation proof, the test set to run before a push, and reading CI.
- [traps.md](references/traps.md): mistakes made in this area and how to
  avoid each.
- [state.md](references/state.md): what has landed and what is open. Update
  it when an item closes or a new gap is proven.

## Before saying done

- The function the user showed prints right in HighC and LLVM C, checked
  against the disassembly or the MSVC source, whole file and alone.
- A same-base corpus sweep is identical except the intended files, and every
  changed file is explained (count notes and definitions, not only files).
- Native or patch changes ran under execution in every mode they affect.
- The test set passed, failures compared by name with the base's.
- Pushed to `dev`, CI dispatched, hand-over build rebuilt, `state.md` updated.
