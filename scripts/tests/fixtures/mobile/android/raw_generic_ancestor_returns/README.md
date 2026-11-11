# Raw generic returns through plain class ancestors

A plain parent implements a raw generic interface. A plain child extends that
parent and implements another raw interface from the same generic hierarchy.
All original class Signatures remain absent; generic interface declarations and
an independently parameterized caller return retain their original Signatures.

The immutable JVM harness compares original parent and caller dispatch,
reflection and MethodHandles, iterator identity and removal state, mutable
aliases, null results/receivers, repeated field effects and exact exceptions.
Recompiled metadata and original runtime entries remain unchanged, apart from
reported private source helpers. Source and immutable ancestor-JAR profiles use
the same oracle. A same-Code method-formal mutation is rejected; independent
javac also rejects that name-clashing raw override.

The `--covariant-target` variant returns an original `NarrowIterator` from the
child. The immutable plain parent and its raw interface still return `Iterator`;
a separate raw generic interface requires `NarrowIterator`. Javac's original
wider bridge must be checked through the proven narrow target. The same harness
wraps its mutable iterator in an owned proxy, so all nine routes retain their
identity, state and exception oracle. Both original method descriptors and bridge
flags survive recompilation; coverage discloses only bridge regeneration. A
same-Code missing-synthetic counterexample must also fail.
