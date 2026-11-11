# Protected scoped void parameters

A final generic class specializes a two-formal abstract superclass with
`Box<E>` and `E`. Its original protected `insert(Box<E>, int, E)` target is
reached through a public erased compiler bridge. The owned input encoding changes
only the original bridge's access flag; Code attributes remain unchanged.

Recovery preserves the generic container and value relationship, scalar index,
all method descriptors and the original bridge. Its only reported method
metadata change promotes the protected typed target to public source access.
Independent callers cover typed, parent, raw bridge, reflected and method-handle
routes with nullable containers and values, extreme indexes, target failures
and invalid erased container types. The matrix checks 293 values, 868 identities,
388 states and 300 ordered failures. Full metadata and exception declarations
are compared outside the disclosed access change.

The source profile retains three classes and eight methods; immutable Base and
Box declarations leave one input class and four methods. Same-Code counterexamples
alter the container's invariant argument or the value formal, or remove the
bridge's synthetic flag. Independent javac rejects the incompatible superclass
argument. Native checks also cover method-owned scope, nonfinal owners, access,
variance and corrupt forwarding registers.
