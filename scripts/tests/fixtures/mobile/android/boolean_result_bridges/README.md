# Boolean callback result bridges

These owned Java inputs encode a final generic callback with distinct JVM
`invoke()Z` and `invoke()Object` entries. The runner changes only the primitive
entry name, the class Signature and original bridge flags after compilation;
all original Code bytes and descriptors remain unchanged. The erased bridge
still calls the primitive target followed by `Boolean.valueOf`.

The immutable harness exercises primitive, erased, direct and reflective calls,
mutable state, exact call counts, cached Boolean identity, target exception
identity, null receivers and source name collisions. Source and SDK interface
profiles compare the complete rebuilt inventory and metadata, permitting only
the reported raw interface Signature, primitive entry name, bridge flags and
generated helpers. Same-Code incompatible result Signatures and independent
Java compiler counterexamples must fail.
