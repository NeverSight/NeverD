# Scoped char carriers with original sibling getters

Two generic factories share an original anonymous class with an enclosing result
formal. The class checks Character, calls charValue and forwards to its original
char overload. A sibling View<Character> getter keeps its exact return Signature.
Owned metadata encoding changes only class/enclosure metadata and the bridge
flags; every Code attribute and runtime descriptor remains unchanged.

The immutable JVM harness compares 336 values, 384 identities, 776 states and 48
failures through eight direct, reflected, MethodHandle and original caller routes.
It checks unsigned char boundaries including surrogate values, separate instance
state, mutable captured results, original getter identity, null/wrong inputs and
exact exception identity after original effects. Rebuilt inventory and metadata
must match except the reported shared member name, original enclosing-formal
source projections, raw interface edge and bridge flags.

Use `--dependency-root` to retain the interface and generic view in an unchanged
JAR. Same-Code changes to the result formal, getter type or synthetic flag must
fail; an independent javac check rejects the conflicting getter contract.
