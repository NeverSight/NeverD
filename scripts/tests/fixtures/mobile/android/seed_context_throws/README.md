# Direct generic seeds with closed declared exceptions

Independently authored static methods retain their sole method-owned Object
formal in one direct parameter and the result, beside a String context and the
actual Object array. Both varargs and fixed-array entries declare an unchecked
exception. The returned reference is chosen by the provider after its callback;
it need not be the seed.

The frozen observer checks 80 fresh JVM cases twice in source and SDK-provider
profiles. It observes ignored calls, late result casts, array identity and
failures, reentrant writes, callback order and exact thrown-object identity.
Every emitted Java unit compiles. Original declarations, metadata, runtime
roles and Exceptions must match; wrong programs and Signature-only controls
check the observer and the admission boundary independently.
