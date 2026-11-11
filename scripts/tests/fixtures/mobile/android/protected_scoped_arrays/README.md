# Protected scoped array parameters

A final Core<E> implements Base<E[], E> with a protected E[] parameter and
Result<E> return. Only its original compiler bridge access is encoded as public;
all Code attributes and runtime descriptors remain unchanged. Recovery keeps the
full class, method and field Signatures and reports the target visibility change.

Run `test_mobile_android_protected_scoped_arrays.py`, optionally with
`--dependency-parent` for immutable Base declarations. An independently compiled
package caller and harness check inherited, typed, reflected and method-handle
routes, constructor effects, nulls, original failures, wrong primitive arrays,
returned container identity and mutable array aliases. Original method/field/
class metadata and Exceptions match outside the disclosed target access change.

Same-Code array-element, result-argument and method-formal scope changes are
rejected, as is a missing synthetic bridge flag. Independent javac rejects an
incompatible parent parameter. Native cases cover one/two dimensions, same and
broader erased return slots, source/dependency parents, ordering, copied input
casts and altered input/result origins.

The JVM matrix checks 65 values, 131 identities, 117 states and 69 ordered
failures. Source/dependency profiles retain three/two classes and seven/four
original methods respectively; two original methods have reported projections.
