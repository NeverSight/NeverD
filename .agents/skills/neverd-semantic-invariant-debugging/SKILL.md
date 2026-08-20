---
name: neverd-semantic-invariant-debugging
description: Diagnoses and fixes NeverD lifting bugs caused by duplicated, order-dependent, or format-divergent semantic decisions across CFG traversal, ABI recovery, pointer provenance, loaders, architectures, and emitters. Use when a new issue resembles a previous fix, a stricter verifier exposes a latent failure, PHI or subregister aliases disagree, or a format-specific reproducer may indicate a shared pipeline defect. Do not use for unrelated feature work or ordinary toolchain failures.
---

# NeverD Semantic-Invariant Debugging

## Goal

Find the semantic question that the pipeline answers inconsistently, give that
question one authoritative implementation, and prove the invariant across every
relevant caller, architecture, and binary format.

A fix is incomplete when it merely makes the reported fixture pass. It is
complete when:

- the failure is explained by evidence from the actual pipeline;
- all implementations of the same decision have been found;
- one shared policy replaces the divergent local policies;
- positive and negative counterexamples lock down the policy; and
- the reported case and its relevant siblings pass without weakening safety
  checks.

## Failure Model

This class of bug commonly has the following shape:

1. Two or more IR values represent aliases of the same machine-level fact.
2. Different recovery paths choose between those values using different rules.
3. The observed result depends on PHI insertion order, block layout, traversal
   order, binary format, or a recent hardening check.
4. A local fix corrects one path while a sibling path retains the old rule.

Examples include:

- a narrow and a full-width register PHI for one ABI argument slot;
- entry-seeded data competing with a wider value that is undef on the first
  loop iteration;
- pointer provenance recovered differently by direct, cross-block, and
  predecessor scans;
- one loader exposing relocation metadata that another loader drops; and
- the same semantic decision being repeated in an emitter and a helper.

Treat a verifier or ambiguity diagnostic as evidence. Do not silence it until
the upstream semantic conflict has been understood.

## 1. Reproduce at the Smallest Faithful Boundary

Preserve the reporter's input and command line. Record:

- commit SHA;
- architecture and binary format;
- exact NeverD command and stage;
- complete diagnostic;
- relevant disassembly;
- MedIR around the failing block; and
- emitted LLVM IR when emission succeeds.

Use the narrowest stage that still exposes the defect:

```bash
build/bin/neverd lift --dump-med <binary> -o /tmp/repro.ll
build/bin/neverd lift --no-opt <binary> -o /tmp/repro.ll
```

Add decompile, patch, recompilation, or execution only when the failure crosses
those boundaries. Do not replace a faithful linked-binary reproducer with an
object file when imports, relocations, section permissions, or ABI metadata are
part of the behavior.

Before changing production code, state one falsifiable hypothesis. For example:

> Cross-block argument recovery selects the first PHI for an ABI slot, so a
> narrow alias wins only because it was inserted before the full-width alias.

## 2. Locate Every Owner of the Semantic Decision

Name the question independently of the current function. Examples:

- "Which PHI is authoritative for ABI argument slot N?"
- "Does this address have pointer provenance?"
- "Which relocation owns this stored value?"
- "Is this frame reload an address or a scalar?"

Search for every place that answers that question. Include:

- direct scans and CFG-backward scans;
- current-block, predecessor-block, and loop-header handling;
- optimized and unoptimized emitters;
- loader-specific metadata producers;
- architecture-specific helpers; and
- fallback or stale-address paths.

Use history to distinguish a new regression from an old inconsistency:

```bash
rg -n '<selector|walker|diagnostic|fallback>' lib include unittests
git log -S'<relevant rule or diagnostic>' -- <paths>
git blame -L <start>,<end> <file>
```

Build a small decision table. If equivalent callers use rules such as
"first match", "widest", and "entry-seeded then widest", the duplicated policy
is the root cause. Changing only the caller reached by the report is not enough.

## 3. Write RED Tests for the Policy, Not Only the Fixture

Before the fix, add a focused test that fails for the hypothesized reason.
Exercise both the reported ordering and its semantic counterexample.

For register or PHI alias selection, cover at least:

| Case | Expected choice |
|---|---|
| Narrow alias appears before an equally seeded wide alias | Wide alias |
| Wide alias appears before an equally seeded narrow alias | Wide alias |
| Wide alias is undef on an entry edge; narrow alias is seeded | Narrow alias |
| PHI predecessor is missing or invalid | Do not treat it as safely seeded |
| Constant incoming value on an entry edge | Treat it as defined |

The first two cases reject insertion-order behavior. The third prevents an
overcorrection such as "always choose widest".

Test the policy below file-format handling when the invariant is format-neutral.
Then retain an exact end-to-end fixture for the reported format so loader,
relocation, ABI, and emission integration remain covered.

## 4. Fix the Authoritative Layer

Create one named helper or model operation for the semantic decision and route
all equivalent callers through it. Remove duplicated local implementations.

For ABI register-PHI aliases, the intended precedence is:

1. reject candidates that do not represent the requested ABI slot;
2. prefer a candidate genuinely defined on every function-entry edge;
3. among equally safe candidates, prefer the widest register view; and
4. never use container order as semantic priority.

This precedence is specific to alias selection. Do not generalize it blindly to
unrelated PHIs or data-flow merges.

Preserve conservative behavior:

- a missing predecessor is not proof of a seeded value;
- a loop-backedge definition does not make the first iteration defined;
- a numeric address alone is not pointer provenance;
- a full-width value is not automatically safe when its entry value is undef;
  and
- an ambiguity diagnostic must not be replaced with a stale-address fallback.

If the decision is made after decoding into shared MedIR, prefer a
format-neutral fix. Use a loader-specific fix only when evidence shows that the
formats produce different or incomplete metadata before the shared boundary.

### Re-materialized immutable table bases

When a nested PHI combines an exact recurrent arm with a reset arm that
re-materializes an immutable table base, read
[references/rematerialized-table-base.md](references/rematerialized-table-base.md)
before changing recurrence or provenance logic. It defines the split proof,
initializer anchoring, fail-closed boundaries, and regression matrix.

## 5. Audit Sibling Surfaces

Expand from the root abstraction, not from superficial text similarity.

### Control-flow surfaces

- call block;
- dominating predecessor;
- loop header and backedge;
- direct and indirect calls; and
- optimized block layouts.

### Register and architecture surfaces

- full register and subregister aliases;
- AArch64 and ARM32;
- x86-64 and x86 when the target-register API exposes the same concept; and
- argument, return, frame, and temporary values where the shared helper applies.

### Binary-format surfaces

- Mach-O;
- ELF; and
- PE/COFF.

Do not assume a bug is format-specific because the reporter supplied one format.
If the conflicting choice occurs in shared MedIR or ABI recovery, build an
equivalent fixture for the other supported formats. Conversely, do not claim an
exact cross-format reproducer unless the same intermediate shape was observed.

## 6. Validate in Layers

Run validation in increasing cost order:

1. policy-level RED tests;
2. the exact reporter fixture;
3. reversed-order and unsafe-wide counterexamples;
4. relevant architecture variants;
5. Mach-O, ELF, and PE/COFF fixtures when the boundary is shared;
6. the owning semantic and pointer-provenance suites;
7. patch or recompilation execution when affected; and
8. the full relevant test binary or CI profile.

For a patchable native fixture, test the produced binary repeatedly under its
normal address-randomization behavior when practical. A successful lift alone
does not prove that a recovered pointer remains valid after recompilation.

Report exact pass, failure, and skip counts. A skip is not coverage. If platform
CI is still running, say so explicitly rather than reporting it as passed.

### Choose the build profile by the evidence needed

Release is the default for broad NeverD semantic validation, not a universal
replacement for Debug. NeverD's Debug configuration is intentionally
unoptimized, and its decode/lift path is substantially slower; the repository
CI runs the normal cross-platform suite in Release.

| Purpose | Preferred profile |
|---|---|
| Focused RED test, assertion failure, source stepping | Debug |
| Broad `NeverDSemanticTests`, lift/recompile, or end-to-end regression | Release |
| Optimized-path diagnosis that still needs symbols | RelWithDebInfo |
| Sanitizer run | Dedicated Debug/sanitizer build |

For a broad semantic run, use a dedicated, known-good Release tree and rebuild
the owning target from the current sources before executing it:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4
```

Do not infer the profile from a directory name. Check `CMAKE_BUILD_TYPE` in its
`CMakeCache.txt`, and ensure the requested target actually rebuilt. Reconfigure
after adding a test source so an old executable cannot silently omit it. If a
reused build tree has inconsistent Ninja/CMake state or unexpectedly starts an
unrelated full rebuild, stop using it as validation evidence and configure a
separate clean tree instead of trying to salvage the authoritative run in
place.

A long run counts only when the process exits normally and prints its final
summary. Do not attach a debugger to the only authoritative suite process to
investigate a long tail: attaching can suspend it, and terminating the debugger
can terminate or invalidate the test run. Reproduce the suspected case in a
separate filtered process. Report an interrupted, timed-out, killed, or
debugger-disturbed run as incomplete even when no failure appeared before the
interruption.

### Keep the validation toolchain matched to NeverD LLVM

Do not use an arbitrary system Clang as the sole oracle for LLVM IR emitted by
NeverD. The repository LLVM fork can carry a different canonical intrinsic
signature from the host toolchain; a host parser error around an overloaded
intrinsic can therefore be version skew rather than invalid NeverD output.
Re-run object emission with the repository's LLVM/Codegen path and use that
result for the relink/runtime check.

Do not rename a canonical LLVM operation to a private `__neverd_*` pseudo
intrinsic merely to make the host compiler accept it. If the required intrinsic
is genuinely absent from the project toolchain, add the real intrinsic to the
LLVM fork and keep neverc's corresponding surface synchronized. Record host
toolchain incompatibility separately from semantic test failures.

## 7. Classify the Change Honestly

Use evidence from history and the failing path:

- **New regression:** the faulty behavior was introduced by a recent change.
- **Latent defect exposed by hardening:** the inconsistent rule predates the
  diagnostic, but a newer verifier stopped unsafe output.
- **Incomplete earlier fix:** history corrected one implementation of the rule
  but left an equivalent caller unchanged.

Do not blame the hardening check merely because it is where the pipeline now
stops. A conservative failure can be the correct exposure of an older bug.

## Root-Cause Acceptance Checklist

Do not call the issue root-fixed until all applicable items are true:

- [ ] The exact failure reproduces on the pre-fix revision.
- [ ] A focused test fails for the predicted semantic reason.
- [ ] All equivalent decision sites have been enumerated.
- [ ] One authoritative policy replaces the divergent implementations.
- [ ] Ordering and negative counterexamples pass.
- [ ] Relevant architectures and formats were tested or explicitly ruled out
      with evidence.
- [ ] The original fixture passes through every affected stage.
- [ ] Safety diagnostics remain enabled.
- [ ] Relevant broad regression suites pass with skips identified.
- [ ] Broad runs completed in a verified build profile; interrupted runs were
      not counted.
- [ ] LLVM IR/object validation used the repository-matched toolchain or any
      host-version mismatch was stated explicitly.
- [ ] The final explanation distinguishes regression, latent defect, and
      incomplete prior fix.

## Anti-Patterns

Reject fixes that:

- special-case a function name, symbol, address, section name, or fixture;
- reorder PHIs so the desired value happens to appear first;
- choose the widest value without checking entry definedness;
- weaken or remove the ambiguity verifier;
- add a stale-address fallback;
- patch only the reported file format when the decision is shared;
- duplicate the same selector in another caller; or
- add broad refactoring unrelated to the proven invariant.

## Delivery

Keep one issue and its regression coverage in one focused commit when the user
has requested that workflow. Push, close issues, or mutate external state only
when authorized. A public issue comment should state the root cause, shared fix,
and validation scope without exposing private internal documentation.

The final handoff should answer four questions directly:

1. Was the report a real bug?
2. Was it a new regression, a latent defect, or an incomplete earlier fix?
3. Which sibling architectures, formats, and paths were checked?
4. What prevents the same semantic rule from diverging again?
