# Result-only value carrier bridge

This independent fixture passes an Object-erased scoped input unchanged to
`apply(S) -> long`, then calls the original static `CarrierB.wrap(long)`.
The runner changes only the class's raw Function Signature to Function<S,
CarrierB> and the original invoke method's bridge/synthetic flags. Every Code
attribute, descriptor and target Signature remains unchanged.

The JVM oracle compares long edge values, unchanged input identities, allocation
and mutation, null inputs, target and factory exception identity and ordering,
and typed calls without extra boxing. The emitted raw interface and changed
bridge flags must be disclosed. Changing apply(S) to apply(V) in Signature only
must fail despite its identical runtime descriptor and body.
