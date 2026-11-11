# Nested invariant factory result

Independently authored fixtures retain `<T> Carrier<List<T>> make(Carrier<T>)`
through an inherited public facade. Both invariant result layers and the input
refer to the same Object-bounded method formal; the carrier's own class formal
is independent. The original callback and returned object remain opaque.

The frozen caller runs 128 fresh JVM cases twice, covering direct, separately
compiled, reflection and method-handle calls. It observes argument capture before
class initialization and callbacks, exact input calls, ignored callback values,
returned outer/inner identities, mutable and immutable contents, late List and
String casts, partial writes and exact failure identity. Source and dependency
profiles compile all generated Java and audit original definitions, metadata,
Exceptions and helper roles. Signature-only controls preserve Code, while
independent wrong implementations must fail. No private application material is
included or executed.
