# Boolean defaults before scoped constructor delegation

Two original constructors supply false and true to a selected boolean overload
while forwarding a complete invariant `Callback<V,K>` contract. An integer
overload records different effects. The callback's two arguments retain their
class scope, and both primitive descriptors remain distinct.

Source and declaration-only profiles compile every recovered Java unit, retain
all original entries and metadata, and run independent direct, compiled caller,
reflection and method-handle observers. The observer checks zero/one defaults,
direct boolean and integer inputs, callbacks, nulls, partial final fields,
effect and failure identity, and late result casts. Wrong programs and
Code-preserving Signature controls run against the same frozen originals.
