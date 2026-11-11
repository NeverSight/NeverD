# Protected scoped array returns

Core<E> implements Base<Result<E>, E[]> with a protected generic container input
and E[] return. Only the original compiler bridge visibility is encoded public;
all Code attributes, descriptors and generic Signatures remain unchanged.
Recovery keeps the original public bridge and reports its target visibility.

Run `test_mobile_android_protected_scoped_array_returns.py`, optionally with
`--dependency-parent` for immutable Base declarations. Six independently compiled
routes compare null receivers/inputs/results, empty and covariant arrays, mutable
result identity, provider call counts, original target/provider failures and
cast-before-call ordering. No replacement array or provider implementation is
allowed. Original class/field/method metadata and Exceptions match outside the
reported target access change and explicitly reported private compiler helpers.

Same-Code parameter, array-element and method-formal drift, and missing synthetic
flags, are rejected. Independent javac rejects an incompatible parent parameter.
Native cases include fixed/adapted inputs, primitive and one/two-dimensional
reference arrays, source/dependency declarations, ordering, copied casts and
corrupted input/result origins.

The matrix checks 252 values, 573 identities, 585 states and 369 ordered failures.
Source/dependency profiles retain three/two classes and eight/five original
methods respectively; two original method rows carry reported projections.
