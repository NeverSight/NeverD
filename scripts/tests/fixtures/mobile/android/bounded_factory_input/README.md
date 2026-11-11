# Bounded producer with an independent fixed result

Owned Java 8 fixture for a private static helper whose sole nominally bounded
method formal appears in a complete producer callback. Its fixed unary factory
result is independent of that formal. A separate auxiliary context and a long
value retain their original descriptors and effects. Frozen external callers
check callback and result identity, nulls, argument evaluation before a null
receiver failure, raw callback casts, partial effects, return snapshots and exact
checked/Error failures. Source and SDK-carrier profiles compare every original
definition and supported declaration and compile every emitted Java unit.

The --unused-bound variant independently compiles the same method formal with
its known nominal class bound while every runtime input/result slot is closed.
The complete declaration bound stays in Signature; runtime callbacks, casts,
wide arguments and returned factory are unchanged. No classfile patch creates
the original variant.
