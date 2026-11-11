# Scalar accessor names beside an inlined generic bridge

The owned Java class has byte, long and double field getters and a complete
comparison implementation in its erased interface method. Constant-pool edits
encode `Api<Core>`, synthetic getter flags, the bridge flag and a shared JVM
getter name distinguished by return descriptors. Every original Code attribute
and all other declaration metadata stay unchanged.

Frozen callers compare authored, encoded and rebuilt programs in fresh JVMs.
They cover both wide words, raw NaN payloads and signed zeros, unsigned byte
comparisons, aliases, direct/reflection/method-handle routes, null/type failures,
side effects and exact RuntimeException/Error identities. Independently compiled
wrong programs must fail before decompilation. Both all-source and imported
Api/Trace layouts compile every emitted Java file and audit every original
class, method, field and Exceptions attribute after only explicit disclosures.
