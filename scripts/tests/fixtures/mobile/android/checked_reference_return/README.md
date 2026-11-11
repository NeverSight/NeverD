# Checked reference return slots

These independently authored Java sources have a `Void` target and a checked
`Result` forwarder. The runner encodes a shared JVM name and the forwarder's
bridge/synthetic flags without changing descriptors or any method Code bytes.
The optional mangled profile encodes a name that Java cannot declare. This is a
controlled classfile witness, not output attributed to another compiler.

A frozen caller and observer compare authored, encoded and recovered execution
through direct calls, reflection and method handles. The target can return null
normally or fail before/after callbacks, so admission cannot depend on an inferred
nonreturning body. Long boundary values, mutable identities, nulls, callback order
and exact exception identity are checked. Inventory and all metadata are compared,
allowing only the disclosed source names and bridge/synthetic flag loss. The
callback dependency variant freezes the original interface bytes. Three independent
compiled wrong-code variants must fail the observer; native tests reject missing
casts, changed registers, unknown ancestors and inherited typed return conflicts.
