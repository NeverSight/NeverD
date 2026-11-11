# Final nested visibility bridges

A public final static member extends a protected static member declared by a
separate known enclosing class. The enclosing class keeps a recursive generic
contract, while both member classes have ordinary closed method descriptors.
The original compiler supplies visibility bridges for exact superclass calls.

Frozen callers outside the defining package exercise direct, public-base,
reflection and method-handle routes, reference and wide arguments, mutable
receiver/argument suppliers, null receivers, partial state, repeated calls and
checked, runtime and Error identity. Independently compiled wrong programs
qualify the observer, including changed final/visibility flags and explicit
source overrides that lose original bridge roles.

All emitted Java must compile. Independent classfile facts compare every
original class and method, access, Code role, Signature, Exceptions and parameter
record. Original and regenerated bridge instructions must match after constant
pool index normalization. SDK providers remain frozen original classfiles.
