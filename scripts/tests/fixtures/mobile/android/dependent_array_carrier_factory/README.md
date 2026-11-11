# Dependent formal in an array carrier result

Authored `<T, E extends T> Carrier<E[]> make(Token<T>, Carrier<E>)` code retains
two complete invariant inputs and the exact dependent array result contract.
The array is opaque: the factory retains actual callbacks and the returned
container, without inferring a component, copying data or reading the result.

Each source/dependency and canonical/interface-bound-position profile runs 192
fresh JVM cases twice through direct, separately compiled, reflection and
method-handle calls. The frozen observer checks both input identities and call
order, ignored values, returned outer/array identities, late subtype-array casts,
null/wrong/empty arrays, callback snapshots and exact checked/runtime/error
identities. Every generated Java unit and all original declarations, metadata,
Exceptions and helper roles are audited. A Signature-only encoding of `E::T`
preserves Code and discloses exactly the existing bound-position normalization
when that declaration is emitted as Java. No private application material is used.
