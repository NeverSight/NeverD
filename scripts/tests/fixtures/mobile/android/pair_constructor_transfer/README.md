# Ordered pair transfer before constructor delegation

The original constructor retains an `int`/`float` configured `HashMap`, an
opaque static `Map<K,V> -> Box<K,V>` call and a selected `this(Box, Function)`
overload. The factory has its own two formals with the same names as its class.
Its returned box may be unrelated to the original map or null. Independent
callers observe allocation failures, factory and constructor effects, partial
final fields, exact callback and output identities, delayed map reads and casts.

Source and declaration-only profiles compile every recovered unit and compare
all original entries, signatures, field metadata and Exceptions. Metadata-only
negative controls retain all original Code bytes. Incorrect programs must be
rejected by the frozen observer before recovery is tested.

Originals compile at release 8. Recovered constructor prologues compile at the
reported minimum release 25 using JDK 25 or newer; the CI profile selects JDK 25.
