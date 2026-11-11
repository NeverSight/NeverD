# Private seeded primitive results

Authored private instance helpers retain `<T> primitive select(Callback<? super T>,
T)` with Boolean, int, long, float and double results. The method's own Object
formal and complete consumer contract remain independent of the receiver and
primitive result. Every original call and callback is retained.

The frozen observer runs 240 fresh JVM cases twice through direct, reflection,
method-handle and original public-wrapper callers. It checks actual receiver,
callback and seed identity, partial state stores, callback updates, captured
primitive results before later field changes, negative zero, NaN payload bits,
null inputs and exact checked/runtime/error identities. Both owned and dependency
consumer declarations are tested while the private helper stays in the DEX.
Every generated Java unit, original declaration, metadata record, Exceptions
attribute and helper role is audited. No private application material is used.
