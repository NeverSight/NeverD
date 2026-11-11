# Private interface-intersection method fixture

The owned Java fixture compiles independently. The oracle replaces one
Signature UTF8 constant with `C: Container<?> & R, R: Object`, preserving every
executable byte and runtime descriptor. The JVM reads the original intersection
bounds; D8 supplies the recovery input and SDK dexdump checks its inventory.

The recovered source reports its erased generic constraints and retains the
private static method's descriptor. Recompilation and JVM comparison cover
reference identity, interface dispatch, query-before-callback ordering, side
effects, exception identity, null behavior, access and generic reflection.
