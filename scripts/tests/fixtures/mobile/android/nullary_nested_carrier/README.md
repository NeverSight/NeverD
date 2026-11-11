# Nullary nested carrier factories

Independently authored source and dependency profiles exercise a public static
`<T> Carrier<T, ?, Box<T>> make()` and the standard `Collectors.toSet()` and
`Collectors.toList()` factories. No application sample code is included.

A frozen observer runs 96 fresh JVM cases twice each, checking actual result
identity, nulls, failed casts, ignored calls, callback mutation and failures,
class initialization and erroneous-class reuse. Standard factory results are
used to collect independently created objects, preserving membership, order,
nulls and identity. Sixteen deliberately wrong programs must fail before the
original DEX is decompiled. Eleven Signature-only controls keep all Code and
runtime descriptors unchanged and must fail closed in both profiles.

The runner independently inventories original class/method definitions, compiles
all emitted Java, checks metadata and runtime roles, and compares the original
and recovered programs against the frozen observer. Only these owned fixtures
execute on the JVM.
