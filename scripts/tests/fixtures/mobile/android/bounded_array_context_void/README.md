# Bounded array input beside closed contexts

Independently authored fixtures retain `<T extends Mark> void write(Context,
String, T[])`, with a plain interface bound and two fixed nominal contexts.
All 96 fresh JVM cases run twice through direct, explicitly typed, reflection
and method-handle callers. They check argument/cast order, exact array identity,
virtual callbacks, captured index/value, covariant stores, partial mutation,
null and wrong inputs, and exact checked/runtime/error identities.

Both source and dependency profiles compile every generated Java unit and audit
all original definitions, signatures, metadata, Exceptions and helper roles.
Signature-only controls retain identical method Code; independent wrong
implementations must fail the frozen caller. No private application material
is used or executed.
