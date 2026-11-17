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
A local check that already proves a jump-table domain is that domain. Do not
replay the same fact through a second walk of the operand's origin, and do not
discard the local result when that walk cannot finish. A reload of the index
in the load block is not an address fact when the address formula already
names one address for every 32-bit index. The guard owns that lane. That
shortcut does not cover a call or store that receives the reloaded cell.
An unproved indirect edge does not restart the incoming stack pointer; a
block it alone enters is still checked against the preserved frame at the
reload. Two reloads of one frame slot are that lane when the reaching store
zero-extends a W write into the stored X. A plain X store is a different
value. An unfinished scan for predicated aliases is not a fact about that
compare. An unfinished
materialization of an unrelated guard is not a fact about a structural index
compare. Neither one withdraws the compare. A definition that already limits
the index to a dense prefix is part of the compare's domain. An equality exit
at the top of that prefix is the bound. An unfinished or inapplicable
definition walk does not invent a larger domain. A candidate that spends the
production personal evidence cap without publishing fails closed for that
branch alone, and the stage keeps siblings whose proofs already completed. A
smaller initial grant means the shared stage account ran out, and that stage
still rolls back. A direct copy of the compared register into the index,
taken before the compare, is that lane, and a later write of the compared
register does not replace it. The load block's zero-extend of the recorded
narrow index is that address identity when it is the last write of the
register before the load and no call sits between them. An earlier
zero-extend is that same identity when every predecessor of the load reaches
it, no block on the way calls or writes the register, and a backedge is not
a second write. An AND that clears every bit above the 32-bit lane is inside
that lane even when the mask is tighter. An unfinished value query does not
withdraw it. An unfinished mask search does not turn a guard that already
rejected the selector into an incomplete proof, and it does not withdraw a
sibling that published in the same stage. A guard that is itself unfinished
still fails closed. A bounded jump-table slot that names an
instruction this function already decoded is that edge, and a slot that only
names the next function stays unresolved.
