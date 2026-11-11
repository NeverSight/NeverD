# Scoped implementations and original unchecked Exceptions

The owned `Api<I>` has an empty exception contract. `Core<G>` retains class-owned
`G read(G)` and method-owned `<R> R unwrap(Class<R>)` with original ordered
RuntimeException and Error declarations. The interface can remain input source
or be an immutable declaration JAR. No bridge or metadata projection is needed.

The immutable `ScopedUncheckedImplementationsHarness.java` checks direct/raw
interface/reflected calls, an independent override, mutable aliases, nulls,
original exception identity and effects before Class casts or null checks.
Each profile checks 48 values, 536 identities, 104 states and 56 ordered failures.
Full compiler metadata and runtime inventory stay unchanged apart from reported
private helpers. The source profile has four classes/seven methods and the
JAR profile three classes/five methods. Coverage is `recovered`.

Changing only a method-owned formal bound preserves Code and runtime descriptors
but fails NeverD and independent javac. Native controls also require both common
closed and unchecked exception receipts and reject checked/mixed/unknown
hierarchies, generic throws, changed parameter scopes and static drift. Original
input, harness, dependency JAR and SDK hashes remain unchanged.
