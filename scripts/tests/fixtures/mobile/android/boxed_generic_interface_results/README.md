# Boxed generic interface results

This authored fixture retains an Object-returning bridge, an effectful primitive
target and a mutable static boxing factory. The runner compiles `Api<Object>`,
changes only the original class Signature to `Api<Box>` and sets the original
`next()` bridge/synthetic flags. Encoding receipts verify unchanged Code and
descriptors; SDK D8 disassembly verifies the exact five-operation wide-result
and factory sequence.

The immutable harness compares 186 values, 610 identities, 484 states and 34
failures across six direct/interface/reflection/MethodHandle routes and two
default callback routes. It covers wide extremes, factory allocation/cache/null
choices, mutable markers, iterator position, target/factory/callback exception
identities and null/remove ordering. The independent method-owned `echo` scope
and empty marker remain exact.

Source and unchanged interface-JAR profiles retain every original runtime entry
and complete metadata, allowing only reported Object interface arguments,
bridge/synthetic flags and private source helpers. Same-Code complete argument
drift and missing synthetic flags are rejected. No typed getter or replacement
boxing implementation is generated.
