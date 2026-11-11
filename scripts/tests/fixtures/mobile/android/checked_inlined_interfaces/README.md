# Checked inlined interface bodies

This independently authored fixture models a compiler bridge with its complete
implementation in the original erased slot and no typed target method. The
runner compiles `Core` against `Api<Object>`, then changes only the class
Signature to `Api<Core>` and the original `read(Object)` flags to public,
bridge and synthetic. The encoding helpers verify unchanged Code bytes and
runtime descriptors. The bridge checks the input, unboxes the argument before
the receiver and compares the two wide values. It never forwards to a typed
`read(Core)` method.

The immutable harness compares 156 values, 774 identities, 570 states and 36
failures across six direct/interface/reflection/MethodHandle routes. It covers
wide extremes, aliases, mutable marker arrays, exceptions after each unboxing,
cast failures, null arguments and null receivers. It separately checks the
reported Object-interface-argument and bridge/synthetic reflection differences.

Both source and unchanged interface-JAR profiles compare the entire original
class/method inventory and metadata, allowing only reported source helpers and
metadata changes. Same-Code class argument drift and missing original synthetic
flags are rejected. No private sample content is included.
