# Implementation principles

[Agent guide](../AGENTS.md) · [C/C++ style and policy ownership](../CONTRIBUTING.md#code-style)

Apply these principles when implementing or reviewing NeverD code.

## Validation and errors

- Validate unknown binary data, persisted state, and CLI/SDK/IPC input at the
  owning reader or entry point. Pass typed, normalized values inward and reuse
  the shared validator across adapters.
- Do not repeat null, type, shape, or range checks for facts already guaranteed
  by a type, constructor, or successful validation. Recheck only when mutation
  or lifetime changes can invalidate that fact, or the next operation requires
  a different precondition. A typed object alone does not prove its ranges valid.
- Malformed and adversarial binaries are expected inputs. Keep necessary
  bounds, overflow, and format checks at their owner. Assertions may express
  internal programmer invariants; they cannot replace runtime input validation.
- Follow the subsystem's error convention, including `llvm::Error` and
  `llvm::Expected` where established. Catch exceptions only for meaningful
  recovery or boundary translation, preserving required C/plugin containment.
  Do not hide invariant violations or turn failures into successful empty or
  default results.

## Keep implementation direct

- Keep cohesive single-use logic at the call site. Extract helpers for actual
  reuse/composition, complex boundaries, or meaningful operations whose names
  clarify the caller. Avoid wrappers and frameworks for hypothetical reuse.
- Inline trivial single-use values when clearer. Keep names that explain meaning,
  evaluation order, ownership, or lifetime. Use `auto` when the type is evident;
  retain explicit types for significant widths, signedness, conversions, and
  interface contracts.
- Use the existing C++/LLVM/Qt and Python facilities. Choose loops or algorithms
  for clarity and appropriate allocation behavior; do not force functional
  chains or introduce a runtime just to imitate another project's style.

## Speculative defenses

Omit hypothetical or exceptionally unlikely race/security machinery by default.
Before proposing such complexity, explain the concrete failure mode, realistic
likelihood, and maintenance cost, and obtain the user's agreement before adding
it. Do not silently introduce retries, fallback paths, arbitrary caps, or
synchronization for "just in case" robustness.

Routine boundary validation and fixes for demonstrated failures within the
requested work do not require separate approval. Existing necessary checks stay
in place; removing one requires identifying the guarantee that makes it redundant.
