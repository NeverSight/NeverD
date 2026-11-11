# Checked synthetic generic bound fixture

The owned Java method accepts Object at runtime, casts the non-null input to
Comparable, then invokes a recursively bounded interface. The independent JVM
baseline changes only the method's synthetic flag and one Signature UTF8 value:
the raw interface bound becomes `Bounded<T>` while `T` remains Object-bounded.
All executable bytes and descriptors remain unchanged.

The D8/JVM oracle checks complete method inventory and recompiles recovered
sources. It compares result and reference identity, null short circuits, the
original cast before a null receiver failure, a later bridge cast, callback
effects and exact exception identity. The source projection explicitly reports
erased generic constraints and the resulting reflection difference.
