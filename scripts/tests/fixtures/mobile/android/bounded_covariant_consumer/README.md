# Bounded covariant carrier consumer

A public final instance entry `<T extends Bound> void accept(Carrier<? extends
T>)` retains the carrier declaration's identical known nominal class bound.
Its owner also inherits an unrelated closed generic parent; the method formal
belongs only to the original method scope. Raw, typed and direct callers retain
the actual input object, callback, partial fields and exceptions. No element is
checked against a narrower caller type by adapting this void invocation.

Source and SDK profiles compile every recovered Java 8 unit and compare all
original identities, class/member metadata, Exceptions and helper roles. Each
runs 64 fresh JVM cases twice, including nulls, callback and Error identity,
mutated inputs, field snapshots and later checked reads. Eight Signature-only
controls preserve Code, and eight independently compiled wrong programs must
be caught by one frozen caller. Only authored fixture code executes.
