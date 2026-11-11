# Private instance field source names

Owned generic receiver objects contain a final reference, mutable callback and
volatile long. A constant-pool spelling-only encoding gives these private fields
Java-inexpressible names, without changing any Code, descriptor, flag or attribute.
The original and encoded programs run under the same frozen observers.

Source and imported-observer layouts compile all emitted Java and reconcile every
original class, method, field and Exceptions attribute, allowing only the three
reported field names. Frozen direct callers, reflection and method handles observe
two separate receivers, constructor partial effects, final reference identity,
null receivers, wide extrema, callback results, reentry and exact checked/runtime/
Error identities. Independent wrong programs must fail these observers. Field-name
reflection and serialized/binary field names are explicitly not regenerated.
No application data is used.
