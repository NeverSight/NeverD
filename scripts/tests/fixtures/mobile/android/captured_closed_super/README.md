# Captures before a closed generic superclass constructor

Genuine Kotlin 2.0.21 value-class output captures a seed callback, reference
carrier and suspend callback before invoking SuspendLambda's closed
`(int, Continuation<Object>)` constructor. The unavailable static class formal
has three explicit source projections; it is not inferred from the parent.

A frozen Java observer exercises callback and holder snapshots, opaque/null/
array values, result identity, suspension, continuation ownership, resumption
and exact runtime/error failures through four invocation routes. Every emitted
Java unit must compile. Original compiler Code and observer bytes stay frozen.
