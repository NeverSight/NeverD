# Nullable owner constructor

Original Java 25 flexible constructor code checks its typed Mutable<S> input,
casts to the declared State<S> ancestor and passes a null Core<?> plus String
label to the same-owner target. Original invariant ancestry and wildcard null
are retained. Frozen direct, precompiled, reflection and method-handle callers
observe the check, type metadata, identities, nulls, partial final fields and
failure order. Four wrong programs must fail the observer; no original bytecode
or metadata is rewritten. The dependency route borrows State and Mutable only.
