# Local allocation before this-delegation

Original Java constructs a local Token with Object/long inputs before the
same-owner constructor initializes final Core fields. Frozen direct,
precompiled, reflection and method-handle callers observe both partially
constructed objects, identity, nulls, wide extrema, final fields and exact
failure/effect order. Four wrong programs must fail the observer. No original
instruction or metadata is rewritten; the SDK variant borrows Token only.
