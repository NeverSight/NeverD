# Recursive Comparable array predicates

An independently owned predicate receives an array of one method formal bounded
by Comparable<? super T>. Frozen direct, separately compiled caller, reflection
and MethodHandle paths preserve the array, its runtime component, comparison
receiver/argument identities, callback mutations and exact failures. Boolean
results stay independent of comparison values; null/wrong boxed values fail only
after original effects. Null/empty arrays, inherited lower-bound implementations,
raw bridge casts and skipped reads are observed in 160 fresh cases, repeated twice.
Source and SDK profiles compile every emitted source and compare original roles,
Signature and Exceptions. Fifteen wrong programs and eight same-Code Signature
controls qualify the observers and recursive-bound rejection boundary.
