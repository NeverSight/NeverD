# Closed reifiable auxiliary beside a method token

Authored class and interface consumers retain `<T> void consume(Token<T>,
Callback<? super List<? extends Carrier<?>>, ? extends Carrier<?>>)`.
The invariant token alone binds T. The complete closed callback tree binds none;
all nested declarations, Object bounds and star slots use one shared validator.

The frozen observer runs 96 fresh JVM cases twice through virtual, interface,
reflection and method-handle callers. It checks original receiver/token/callback
identities, future overriding dispatch, exact call order, ignored token values,
null/wrong/poison/opaque list inputs, callback return identity, partial field
stores and exact failures. Both source and dependency profiles compile all
recovered Java and audit every original definition, metadata record, Exceptions
attribute and helper role. No private application material is included or run.
