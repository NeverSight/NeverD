# Bounded factory with two direct seeds

All Java sources and frozen observers are independently authored here. The
original factory retains two method formals, an invariant bounded converter,
an invariant seed carrier, two direct seed values, a contravariant callback
with a fixed nominal result and an invariant returned carrier. It has no
application data. D8 preserves the original class and method inventory.

The runner compares original and recovered source with immutable typed,
reflection and method-handle callers. Real converter/callback implementations
live only in the frozen observer; arbitrary and null returns, late bridge
casts, partial effects and exact failures remain observable.
