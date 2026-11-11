# Default scoped covariance

The unmodified Kotlin compiler creates a stateless raw callback whose typed
parameter refers to an enclosing generic method through its synthetic default
entry. Frozen JVM callers observe both original callback descriptors,
raw dispatch, exact method bounds, singleton identities, partial effects and
exception identity. All sources are independently authored; no application data
is included.

The inline stage object supplies the existing scope resolver with a constructor
witness in both original method bodies. To compile a direct JVM caller of the
synthetic default entry, a separate declaration view clears only its synthetic
access bit and proves unchanged Code. That view is never executed or recovered;
the immutable caller runs against original and rebuilt classes.
