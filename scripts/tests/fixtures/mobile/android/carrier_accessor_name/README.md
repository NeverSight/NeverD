# Scalar carrier accessor name

A genuine Kotlin value class implements Comparable and exposes its original
synthetic unbox method. Its checked erased comparison bridge already owns the
raw source Comparable projection. The unrelated final scalar field getter
receives only a disclosed Java spelling. Frozen callers compare raw float bits
including NaN payloads and signed zero, comparison dispatch, null/wrong-type
failures and effect order. Four wrong programs must fail. Every original entry,
Signature, flag and annotation is checked after exact disclosed projections;
D8 CLASS annotation loss is recorded separately.

The `--primitive long` and `--primitive double` profiles compile authored Kotlin
variants and frozen Java observers before decompilation. Their D8 witnesses keep
the two result words separate from the incoming receiver. Each profile covers
64 fresh JVM cases twice per original/rebuilt stage: long extrema and both word
halves, double NaN payloads, signed zeros, infinities and subnormals, direct and
reflective callers, comparison dispatch, null/type failures and exact exception
identity. Six independently compiled wrong programs must fail before recovery,
including lost word width or canonicalized NaNs. `--dependency-trace` keeps the
original tracing class bytes in a classpath jar; the complete emitted source set
must still compile and retain every original declaration and disclosed change.
