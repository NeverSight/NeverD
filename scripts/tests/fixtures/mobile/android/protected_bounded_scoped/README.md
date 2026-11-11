# Protected consumers with scoped bounded parameters

Core<E> inherits Base<Box<E>, E>, where the first Base formal is bounded by
Container<V>. Box<T> implements Container<T>; the class-bound profile changes
that edge to class inheritance before compiling the owned baseline. The original
protected insert(Box<E>, int, E) has a public synthetic bridge whose erased
parameter is Container, with its original checked cast and unchanged arguments.

Only that original bridge's access is encoded. Independent javac callers and
reflection/method-handle routes run before and after recovery, comparing mutable
aliases, null handling, failed casts to a different Container implementation and
ordered target failures. The original generic bounds and all class, method,
field and Exceptions metadata remain unchanged except the disclosed typed access
promotion. The public bridge stays at the original bounded erased descriptor.

Source and immutable dependency profiles test interface and class bounds.
Each JVM matrix checks 296 values, 868 identities, 388 states and 300 failures.
Native counterexamples reject unrelated bounds, wrong invariant arguments,
method scopes, discarded value scope, wrong casts and argument origins. Encoded
class-file counterexamples keep every original Code byte unchanged.
