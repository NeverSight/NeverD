# Invariant producer and mapped result

Independently authored `fixture.*` classes keep a complete `Iterable<T>` producer,
`Function<? super T, ? extends R>` callback and `List<R>` result beside boolean
and wide scalar inputs. The provider reads one original iterator value, invokes
the callback, then returns an opaque result after another callback. The type
relation predicts no collection algorithm, result contents or returned identity.

Source and dependency profiles compile every emitted Java8 definition and check
all original Signatures, roles, Exceptions and helper accounting. Seven routes
and twenty modes run twice in fresh JVMs: used, narrowed, ignored, relayed,
typed, wide and inherited static calls; nulls, independent/aliased results,
iterator/mapper/action failures, callback mutations and exact reference/order
observations. Independent wrong programs exercise the observer before recovery.
Signature-only negative controls preserve Code and runtime descriptors while
breaking the scoped producer/callback/result relation or declared erasures.
