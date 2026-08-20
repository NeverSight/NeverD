# Re-materialized Immutable Table Bases

Use this reference when a loop-carried pointer is rejected because one feasible
backedge arm re-materializes a relocation-backed table base instead of carrying
the exact outer-PHI SSA value.

## The invariant

A loop-carried pointer may be recurrent without every feasible backedge arm
being the same SSA value. A nested PHI can combine an exact reference to the
outer PHI with a reset arm that purely re-materializes the same table base. An
exact-dependency-only walker rejects that shape before the indexed or induction
resolver can own the address.

Do not solve this by declaring equal integers to be equal pointers. Model two
facts separately while walking the complete incoming expression:

- **Preserves the pointer:** every feasible value arm carries either the exact
  recurrent value or the one independently established immutable base.
- **Reaches the recurrent root:** at least one value path still reaches the
  exact outer-PHI output.

Accept the incoming value only when both facts hold. Establish the permitted
base identity from every proven-feasible, non-recurrent initializer of the
outer PHI; a reset or failing backedge arm must never nominate the identity that
would excuse itself. Multiple initializers must agree regardless of argument
order.

Use the emitter's shared address-materialization and object-provenance policy
when comparing bases. Require:

- exact target-pointer width at every identity-forwarding step;
- the same loader-mapped, immutable, materializable object-data address;
- the same raw-versus-symbolized emission model; and
- only pure pointer-preserving forwarders between the value and its constant
  leaf.

Handle nested PHIs and selects structurally: every proven-feasible selectable
value arm must preserve the pointer, and an unknown edge keeps the proof
fail-closed. Reject different bases, arithmetic that merely computes the same
number, loads, truncation or narrow-then-widen paths, mixed raw/symbolized arms,
unresolved addresses, and scalar arms. Keep wide/narrow register-alias
recurrence on its exact proof unless independent evidence justifies widening
this invariant.

## Regression matrix

Cover:

- the re-materialized backedge and an exact identity-forwarding control;
- AArch64 and x86-64 when the defect is in shared MedIR;
- a different object base and an arithmetic numeric-coincidence counterexample;
- a narrow-then-widen counterexample;
- mixed raw/symbolized initializers in both PHI argument orders; and
- an exact linked-binary lift, object emission, relink, and repeated execution
  when address randomization is part of the risk.
