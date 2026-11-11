# Recursive pair result

Independently authored Java 8 keeps two direct inputs and an opaque result in one
method-owned `E extends Comparable<? super E>` contract. A public facade inherits
the static declaration from a package-private provider. The provider publishes
both inputs, invokes an external observer, and returns a separately supplied
value; the bound does not imply a comparison or selection of either input.

Frozen direct, compiled-caller, reflection and MethodHandle routes check both raw
and later checked results. Source and SDK layouts compile every emitted unit and
compare every original class and method, Code presence, roles, generic metadata,
Exceptions and MethodParameters. Cases cover null and aliased inputs, opaque and
null results, original provider/caller casts, partial writes, repeated calls and
exact runtime/Error identity. Wrong-program and Code-preserving Signature controls
verify that the observer and the binding reject meaningful deviations. These
fixtures contain no private application code or identifying information.
