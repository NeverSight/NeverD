# Bounded owner constructor label forwarding

Independent Java sources retain a two-formal class, a bounded binary converter,
two direct Object seeds and a fixed string label. The three-argument original
constructor has no Signature. A witnessed access-only patch adds its synthetic
flag; all original Code, Signatures and descriptors remain byte-identical.
The immutable observer compares original and recovered construction through
source, frozen callers, reflection and method handles, including partial final
fields and exact exceptions. No application data is present.

The `--mask --interface-owner` profile also preserves an independently declared
unary interface using the owner's Object formal. Original constructors, flags,
field effects and interface dispatch are checked together; interface/class
metadata and all Code attributes stay unchanged after original compilation,
apart from the witnessed synthetic constructor flags.
