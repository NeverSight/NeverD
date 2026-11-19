# Proving an SEH change

## Same-base corpus sweeps

1. Build two engines that differ only by the change: the base commit and the
   change on top of it. Copy each `bin/neverd` and `bin/libneverd.so` into its
   own directory before sweeping; a rebuild under a running sweep mixes
   engines. Sweeps taken before a rebase onto a newer `dev` are not
   comparable with ones after it.
2. Decompile every file of `unittests/corpus/corpus/windows-eh/` (384) whole,
   in HighC and in LLVM C (`--llvm`), with each engine, a few files at a
   time under `nice`, a timeout per file, and the elapsed time recorded per
   file (the same run gives the speed comparison).
3. Compare byte for byte. Every changed file needs an explanation; classify
   the hunks by script (what the line held before, what it holds now) and
   read a sample of each class. Count notes, `__builtin_trap`s,
   `neverd.not-decompiled` lines and definitions, not only files: "every file
   prints" is not "every function prints".
4. To separate lowering changes from optimizer flips, sweep both engines with
   `--no-opt`; a classifier change also needs a `lift` and `patch` sweep that
   shows no new patch success.
5. Real images beyond the corpus: Windows system DLLs (2012 x64 files with
   C++ handlers in one local sample) and a 3000-function ntoskrnl sample
   with `--func` per function.

## Runtime proof

- Printed C: compile the printed function with clang for
  `x86_64-pc-windows-msvc` and run it beside the original under wine64
  (`scripts/check_llvmc_seh_layout_runtime.py` does this for the shared
  `__try` layout, 7 functions x 128 inputs).
- Native output and patches: patch the image in every mode the change
  touches (`--mode=section`, `--mode=inplace`), run original and patched, and
  diff the transcripts. A test program that only exits 0 proves little;
  print each test's result.
- Local unwinds across rows need real Windows: wine lacks a rule of
  `__C_specific_handler` (semantics.md). Build the fixture with MSVC on a
  throwaway `ci/` branch job, download the artifact, cancel the run, then
  run it in wine for the cases wine handles and on Windows for the rest.
- A freestanding Windows toolchain that works on Linux: clang
  `--target=x86_64-pc-windows-msvc -ffreestanding`, `lld-link /nodefaultlib
  /entry:...`, import libraries from `llvm-dlltool` and a `.def` file.
- CFG fixtures: compile with `-Xclang -cfguard`; link without `/guard:cf`
  for a CF_INSTRUMENTED-only image and with it for an enforced one; provide
  `_load_config_used` yourself under `/nodefaultlib`
  (`unittests/lift/x86_64/test_patch_coff_guard.c`).
- One function at a time: lift a single function to native IR (a private
  gtest driver linked against the build's archives, never committed), patch
  it with `neverd patch IMAGE --from-ir=F.ll --mode=section|inplace` (with
  and without `--no-opt`), run the image under wine and diff its transcript
  with the original's. Sweep every SEH function of the MSVC `xcpt4` images
  this way, both lift modes, and count identical runs, refusals by reason and
  any different run. The `local_unwind_raise_runtime.c` fixture covers
  local unwinds across rows: MSVC builds it at /Od, /O1 and /O2, each with
  and without /GS; the /O1 and /O2 builds rewrite (two forms, two lift
  modes, section and in-place, with and without patch optimization: 64
  runs), and at /Od the unwinds are not certified, so `patch` refuses.
- Before accepting a new native shape, sweep `neverd patch` over the whole
  corpus with the old and the new engine: newly accepted files must run, and
  no file may newly refuse.

## Mutation proof of a new test

Break the change one decision at a time (each condition, each branch of the
new rule), rebuild only the test, and require the new test to fail for each.
Report the ones it does not catch. The harness edits the working tree in
place: do not read or build those files until it prints its done line, and
remember that a statically linked test binary ignores `LD_LIBRARY_PATH`.

## The test set before a push

Run the whole set, not one suite, and compare failures by name with the base
commit's list:

- `NeverDLiftTests` (all), the HighC, LLVM C and Windows EH test programs
  (`NeverDWindowsEHSourceTests`, `NeverDWindowsEHCorpusTests`,
  `NeverDSEHTerminationHandlerTests`, `NeverDLLVMCValueTests`, ...).
- When LLVM C or the source map changes: `NeverDLibraryRecognitionTests`
  (needs the `signatures` submodule and a reconfigure),
  `NeverDSourceAnchorTests`, `NeverDWebSourceMapTests`.
- When a patch route changes: `NeverDSemanticTests` filtered to
  `*Inplace*:*BinaryPatcher*:*Interworking*:*Rewrite*`, and the whole of
  `NeverDPatchFullTests` (about two hours here; CI's Linux job excludes it).
- `NeverDSessionCAPITests` whenever LLVM C or HighC printing changes: its
  UEFI and typed-member cases caught what the LLVM C unit tests did not.
- Repository checks: `scripts/check_clang_format.py --base origin/dev --head
  HEAD` with the pinned clang-format, `scripts/check_source_provenance.py`,
  `scripts/check_docs_i18n.py`, `python3 -m unittest
  scripts.tests.test_ci_configuration`.

## Reading CI

- `ci.yml` is dispatch-only. Dispatch on a `ci/` ref equal to the pushed
  commit with `-f use_prebuilt_llvm=true`; never edit the workflow on a
  throwaway branch.
- Linux runners are AMD or Intel at random, and 21 x86 legacy native tests
  fail on AMD only; compare failing names with the last run on the same CPU
  vendor, after stripping `GetParam()` text. ctest lists a test once per
  executable that registers it.
- macOS runners have run out of disk (`ld: write() failed, errno=28`); the
  Windows job has not finished inside its 6 hours. Say which platforms gave
  no verdict instead of calling the run green.
