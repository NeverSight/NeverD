# LLVM-Style Value-Flow Hardening Design

## Context

The cross-platform CI repair at commits `5a675e7` and `99c9e5b` restored the
affected semantic behavior locally: the complete 44,883-test suite passed, and
CodeQL completed with no open alerts. During maintainability review, the i386
variadic detector was found to compute a monotone SSA-value closure by rescanning
the entire function up to an unexplained `100000` iterations.

The same pattern also exists in two neighboring calling-convention analyses:

- `detectXMMParams` in `MedCallingConvX86.cpp`;
- `liveInOnlyFeedsScratch` in `MedCallingConv.cpp`; and
- i386 direct `va_list` walk detection in `MedVariadic.cpp`.

All three analyses operate over a finite set of MedIR SSA values. A numeric
iteration guard is therefore neither the proof of termination nor an acceptable
failure mode: reaching it silently returns a partial analysis result. Repeatedly
rescanning all operations also makes the running time depend on block order and
can become quadratic for long def-use chains.

This change will replace those loops with a bounded worklist closure and perform
a focused LLVM-style review of the eight files changed by the CI repair. The
review is evidence-driven: a file is not changed merely to make the diff look
uniform.

## Goals

- Remove every `100000` fixed-point guard from the three touched
  calling-convention source files.
- Make termination structural: each finite SSA value is inserted into a
  worklist at most once per analysis state.
- Make propagation independent of MedIR block order.
- Preserve the exact existing meaning of PHIs, transparent value forwards,
  self-cancelling operations, partial-register reconstruction, and constant
  `va_list` pointer advances.
- Follow the LLVM coding standards used by the pinned LLVM tree: LLVM ADTs when
  they fit, 80-column formatting, early exits, range loops, explicit invariants,
  and no silent recovery from impossible target widths.
- Remove small pieces of duplicated policy that already caused or could cause
  the two ABI-recovery passes to diverge.
- Retain the currently passing cross-platform and semantic behavior.

## Non-Goals

- A general-purpose data-flow framework for every NeverD analysis.
- Rewriting unrelated calling-convention recovery or jump-table algorithms.
- Changing the MedIR value identity model.
- Changing ABI heuristics, supported architectures, public command-line
  behavior, or test expectations.
- Mechanical conversion of all standard containers in the repository to LLVM
  containers.
- Editing generated semantic test sources or weakening any CI gate.

## Value Identity and Invariants

The worklist key is the existing MedIR SSA identity:

```text
(MedVar::VarKind, Id, SSAVer)
```

Constants are never graph vertices. `Size`, `RegOff`, and the union payload are
not part of the key because `MedVar::operator==` already defines non-constant
identity by kind, ID, and SSA version. The implementation must not introduce a
second, incompatible identity rule.

The closure is monotone:

1. The graph contains only values and forwarding edges present in one `MedFunc`.
2. A value moves only from unseen to reached.
3. Reached values are never removed.
4. A value is queued only when insertion into the reached set succeeds.

These invariants prove termination after at most `V` successful worklist
insertions for one-state reachability, where `V` is the number of trackable SSA
values. No numeric escape hatch is needed.

## Focused Closure Utility

Extend the internal `med_calling_conv_detail` interface with one focused helper,
not a generic solver hierarchy:

- an internal `ValueKey` alias and `ValueSet`;
- a canonical `valueKey(const MedVar &)` function;
- a membership query that rejects constants; and
- `computeForwardValueClosure`, which accepts a function, seed values, and a
  `function_ref` predicate identifying which operation input is transparently
  forwarded to that operation's output.

The predicate contract is equivalent to:

```c++
llvm::function_ref<bool(const MedOp &Op, unsigned InputIndex)>
```

It is called only for an existing input of an operation with a non-constant
output. Returning true creates the edge `Op.Inputs[InputIndex] -> Op.Output`;
returning false has no side effect.

The helper will:

1. Build the forward adjacency map once.
2. Add every non-constant PHI argument-to-output edge automatically.
3. Ask the caller predicate about ordinary operation inputs, so each analysis
   retains its own semantics.
4. Seed an `llvm::DenseSet<ValueKey>` and process an
   `llvm::SmallVector<ValueKey>` worklist until empty.
5. Return the completed reached set for a separate consumer-classification
   pass.

Separating closure from consumer classification is deliberate. It prevents an
operation's interpretation from depending on whether a predecessor happens to
appear earlier in `Func.Blocks`.

## Analysis Migrations

### XMM Live-In Use

`detectXMMParams` will compute the complete closure through PHIs, copies,
integer extensions, zero-offset `SUBBYTES`, and entry self-copies. It will then
scan consumers once:

- self-cancelling `x ^ x` and `x - x` discard the tainted value;
- same-domain transparent forwards do not prove a parameter use;
- an FP-to-GPR reinterpretation remains a genuine use; and
- every other consumer proves the incoming FP value is used.

### Scratch-Only Parameter Detection

`liveInOnlyFeedsScratch` will compute its closure only through the narrower
forwards it currently accepts: same-register copies and width-preserving views.
After closure, one scan will reject any genuine consumer while continuing to
recognize partial-register preserve masks and BSR/BSF old-destination preserves
as scratch reconstruction.

Definition lookup used by BSR/BSF classification should be indexed once by
`ValueKey` rather than repeatedly scanning all blocks.

### i386 Variadic Pointer Walk

The i386 detector has two facts:

- `Direct`: derived from a valid entry-SP-positive pointer stored into a home
  slot; and
- `Advanced`: derived from at least one constant pointer add/subtract and then
  used as a load address.

It will compute them without a two-bit generic solver:

1. Compute `Direct` closure through transparent forwards and constant pointer
   advances.
2. Collect outputs of qualifying add/subtract operations whose relevant input
   is in `Direct`.
3. Compute `Advanced` closure from those outputs through transparent forwards
   and PHIs.
4. Mark the direct-register walk only when a load address is in `Advanced`.

Because the complete `Direct` closure is known before advanced seeds are
collected, multiple advances and loop-carried PHIs are handled without scan
order dependence.

A qualifying constant advance is exactly one of:

- `INT_ADD(value, constant)` from input 0;
- `INT_ADD(constant, value)` from input 1; or
- `INT_SUB(value, constant)` from input 0.

Subtraction from a constant, non-constant arithmetic, and a zero-offset
`SUBBYTES` with missing operands do not qualify. `COPY`, `INT_ZEXT`,
`INT_SEXT`, and `SUBBYTES(value, 0)` are the transparent ordinary-operation
forwards used by both `Direct` and `Advanced`; PHIs are handled by the utility.

## Focused Eight-File Review

The maintenance review covers the production and build files changed by
`6f6e4cf..99c9e5b`:

| Area | Review action |
|---|---|
| `MedVariadic.cpp` | Replace the fixed-point scan and extract readable i386 walk predicates. |
| `MedCallingConv.cpp` | Replace the fixed-point scan, index definitions once, and host the shared detail helper. |
| `MedCallingConvX86.cpp` | Replace the fixed-point scan and share the variadic-overflow boundary predicate used by aligned and raw stack loads. |
| `Pipeline.cpp` | Centralize the rule that a detected variadic callee has open call-recovery arity, then use it in both recovery passes. |
| `JumpTableResolverSource.cpp` | Centralize byte-width truncation used by folded add/subtract/subbytes results. |
| `MedLLVMSwitch.cpp` | Assert the supported nonzero machine pointer width before narrowing a switch index. |
| `MedLLVMX86ValueEmitter.cpp` | Assert the x86 address-register width is 32 or 64 before constructing an LLVM integer type. |
| `unittests/semantic/CMakeLists.txt` | Verify the Windows-only static link is narrowly scoped; leave it unchanged if no concrete defect exists. |

Any additional edit requires a concrete Symptom -> Source -> Consequence ->
Remedy finding tied to this CI repair. Broad stylistic churn is excluded.

Two supporting files outside the original eight-file diff are explicitly in
scope: `include/neverd/ir/med/MedCallingConvDetail.h` for the internal closure
API and `unittests/lift/MedCallingConvTests.cpp` for focused primitive tests.

## Error Handling

- A finite worklist reaching empty is the only normal completion condition.
- Constants and malformed non-values are ignored as graph vertices.
- Operations with missing required inputs do not create forwarding edges.
- Unsupported or zero target pointer widths are programmer errors and should
  fail an assertion close to LLVM type construction instead of silently
  producing incorrect IR.
- Analysis remains conservative: an unrecognized operation is a consumer, not
  an implicitly transparent forward.

## Testing

Validation proceeds from the new primitive to the complete product:

1. Add focused `MedCallingConvTests` coverage for:
   - a forward chain whose definitions appear in reverse block order;
   - a PHI cycle, proving finite termination and complete reachability; and
   - a non-forwarding operation, proving propagation stops at the boundary.
2. Run the calling-convention component tests.
3. Run the i386 variadic regressions that previously exposed truncation,
   including `vargp`, `vargp2`, and `vll`.
4. Run the affected variadic, XMM, partial-register, BSR/BSF, REP-string, and
   jump-table semantic families.
5. Build all default targets and run the complete CTest inventory. Before this
   change the baseline is 44,883 passing tests with only the two documented
   optional real-binary parity skips; new GoogleTest cases may increase the
   final inventory and must pass as well.
6. Run formatting and `git diff --check`.
7. Push only after local validation, then require the latest Linux x64, macOS
   arm64, Windows x64, and all CodeQL language jobs to succeed.
8. Query GitHub code scanning and require zero open alerts.

## Acceptance Criteria

- No `100000` or `10000` fixed-point guard remains in the three calling-
  convention implementation files.
- Worklist termination follows directly from finite-set insertion.
- The three migrated analyses preserve their existing regression behavior.
- Both i386 ABI-recovery passes call the same total-arity policy helper.
- Repeated integer-width truncation logic is expressed once per relevant
  module.
- New target-width assumptions have diagnostic assertions.
- Focused tests, the complete local suite, the three CI hosts, and CodeQL all
  pass.
- GitHub reports zero open CodeQL alerts.
- The final `dev` worktree is clean and synchronized with `origin/dev`.

## Trade-Offs

Building an adjacency map uses temporary memory proportional to the function's
value-flow edges, but removes repeated whole-function scans and gives predictable
`O(V + E)` closure. A reusable detail helper adds a small internal API surface;
this is justified by three independent callers with the same termination and
ordering requirements. A more configurable lattice framework could reduce a few
lines in the variadic detector, but would add abstractions not required by the
current analyses and is intentionally deferred.
