# Anchored method seed and fixed nominal result

Owned source and SDK declarations retain an abstract instance method with one
invariant `Token<T>` input, the same direct method-owned `T`, a fixed nominal
result and an original checked exception. Its concrete implementation and static
caller wrappers use the same binding. Frozen callers and future overrides check
input identity, receiver effects, nulls, dispatch, opaque or discarded results,
original cast timing, partial writes and exact exception identity. Every emitted
Java unit compiles and original definitions, Code/access roles, Signatures and
Exceptions are compared independently. Wrong programs qualify the observer;
same-Code Signature mutations must be rejected without output.
