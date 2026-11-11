# Closed prefixes on genuine suspend interface forwarders

Unmodified Kotlin 2.0.21 JVM8 output contains two generic default interface
methods and their static synthetic bodies. The latter take an explicit closed
receiver and a long or double before the paired Function2/Continuation tail.
The callback's nested consumer and final completion share the sole method formal;
the actual Object result can be arbitrary, null or a suspension marker.

Source and SDK-context profiles compile every emitted Java9 unit (the reported interface-helper minimum) and independently
compare every original definition, complete supported declaration metadata and
Exceptions. Only explicitly reported synthetic flag loss and independently
inventoried D8 CLASS annotation loss are permitted. Original classfiles, DEX,
external callers and independent assertions are frozen before recovery.

Each profile runs 168 fresh JVM cases twice through reflection, two MethodHandle
routes and direct Java. Assertions cover receivers/subclasses, virtual context
lookup, long/double raw bits (including NaN and signed zero), null callbacks and
completions, returned/resumed object identity, partial observations and exact
runtime/error timing. Sixteen separately compiled wrong programs must fail first.
