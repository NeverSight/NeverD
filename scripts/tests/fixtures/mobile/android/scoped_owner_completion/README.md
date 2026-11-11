Owned Kotlin/JVM fixture for a private captured inner class with an unused class
formal and a separate method formal in a paired callback/completion signature.
The public relay casts an opaque session, passes an exact long and preserves the
session's original outer object. Source and SDK interface declarations are tested.

The frozen external Java observer calls through reflection, flexible and exact
method handles, and ordinary bytecode. It checks two callback classes, same and
different caller/creator objects, null and unrelated sessions, callback and
completion identities, raw long values, null/arbitrary/aliased/suspension results,
partial writes, call order, and runtime/error identity. Wrong owned programs must
be rejected before decompilation. Only owned fixture code is executed.

The existing captured-inner source binding makes the member static while keeping
its explicit outer field and constructor descriptor. Its receipt accounts for
the static flag, synthetic-field loss and the now redundant constructor Signature;
the independent method Signature must remain byte-for-byte identical. D8 drops
only the compiler's CLASS-retention nullability and JvmField annotations.

The captured constructor stores its outer field before the super call; all
emitted units therefore compile at the reported Java25 minimum.
