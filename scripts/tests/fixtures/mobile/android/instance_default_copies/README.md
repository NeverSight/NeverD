# Copied instance default receiver fixture

This owned variation of the instance default scope fixture holds seventeen
independent object values across the receiver call. D8 moves its high incoming
receiver into a low entry register in both the original method and its inlined
synthetic default helper. SDK disassembly must independently confirm those
copies and the exact zero-argument member calls on their destinations.

Only the same helper and callback metadata records as the simpler fixture are
changed; javac method bytecode remains intact. The JVM comparison verifies all
272 live local identities across sixteen successful calls, supplied and default
callback identity, mutable callback values, receiver exception identity, null
failures, runtime descriptors and the separately reported reflection change.
Unit rejection cases cover missing entry copies, unrelated inputs, alias and
incoming-register overwrites, overlapping wide writes, late branch copies and
static member declarations.
