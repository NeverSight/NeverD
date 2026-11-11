# Long arithmetic in default constructor prefixes

Unmodified Kotlin JVM 8 output computes a default packed size from two interface
getters before delegating to the primary constructor. Long shifts, conversions,
bitwise operations and effectful calls retain their original typed values and
order, including the one-word shift distance next to a wide result.

Source and SDK-interface profiles compile every emitted Java unit and compare
all definitions, metadata and Exceptions. Frozen observers run 132 fresh JVM
cases twice across reflection, two MethodHandle routes and genuine Kotlin
callers. They check signed boundary bits, getter order, earlier snapshots,
partial initialization, a null receiver and runtime/error failure identity.
Sixteen independently compiled wrong programs challenge the observer before
recovery. Every original classfile, observer and DEX is retained unchanged.
