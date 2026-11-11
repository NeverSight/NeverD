# Class-owned generic bridge implementations

The independently authored fixture retains a class-owned `T` in an original
boolean implementation marked ACC_BRIDGE. A second bridge forwards a zero-argument
predicate to an original static helper. Helpers share a single recursive method
formal with an invariant interface receiver. The encoder changes only the two
bridge flags; original Code, Signatures and runtime descriptors stay intact.

The immutable JVM harness was compiled before encoding. Eight call routes and an
independent subclass compare 112 values, 535 identities, 416 states and 80 ordered
failures. They cover callback override selection, aliasing, state changes before
comparison, null and wrong-type inputs, exact callback/comparison exceptions and
generic reflection. Rebuilt metadata differs only in the two reported bridge
flags. The dependency profile uses an unchanged interface/helper JAR.

A same-Code mutation replaces `T` with unrelated, identically erased `U` and must
fail recovery. An independent javac source with that conflict must also fail.
Run `scripts/test_mobile_android_class_formal_implementation_bridges.py` with the
standard Android/JDK paths, optionally adding `--dependency-root`.
