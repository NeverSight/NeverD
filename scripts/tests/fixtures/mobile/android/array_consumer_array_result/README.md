# Scoped array and unary consumer array results

This independently owned fixture keeps one complete method-owned Object formal
in an input array, an invariant unary consumer and the actual returned array.
Its generic provider owner uses the same formal name with a Number bound, so
method scope must remain independent. Both parameter orders are exercised.

Frozen observers run 120 fresh JVM cases twice, checking null short-circuit
identity, optional callbacks and their values, arbitrary/null/invalid results,
array and consumer identity, ignored calls, original provider/caller array casts,
reentrant writes, array-store failures and exact failure order and identity.
Twenty wrong programs fail before recovery. Source and dependency profiles
compile every emitted Java8 unit and retain all definitions, metadata, runtime
roles and Exceptions. Eight Signature-only controls preserve original Code.
