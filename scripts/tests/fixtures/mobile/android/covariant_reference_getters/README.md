# Complete class-formal covariance and original getter result cast

Receiver<S,V> extends Parent<S>; its create method returns Leaf<S>, whose
complete nominal ancestry implements Base<S>. The original compiler bridge
returns Base and receives one recorded CHECKCAST Base Code edit. All original
descriptors, flags, generic signatures and other Code attributes stay unchanged.

The JVM oracle checks typed/erased/reflective calls, null and container identity,
subclass dispatch, exception identity, mutable payloads and exact generic
reflection. A Leaf<T> -> Base<Object> ancestry Signature edit with unchanged
Code and erasures must fail in NeverD and javac.
