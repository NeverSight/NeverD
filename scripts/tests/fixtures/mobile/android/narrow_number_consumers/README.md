# Narrow Number consumers

The byte and short profiles retain a closed inherited consumer while preserving
its producer, metadata and marker contracts. Each original bridge checks Number
and invokes byteValue or shortValue virtually. Recovery must preserve that
conversion rather than narrow the accepted reference to Byte or Short.

Original encoding changes only the authored raw interface Signature and the
existing consumer's bridge/synthetic flags; its Code attribute is unchanged.
The reported raw source edge and concrete forwarding method preserve the original
runtime descriptors, while producer bridges and the separate marker stay intact.
Full class/method/field/Exceptions inventories are compared after recompilation.

Immutable callers and Number subclasses exercise custom conversion overrides,
Number's default conversion through intValue, overflow, wide integer boundaries,
floating values, NaN, infinities, nulls and ordered exceptions. Six invocation
routes preserve conversion side effects before target effects. Invariant sibling
and consumer binding changes and missing synthetic flags remain errors; javac
also rejects an incompatible specialization.

Each source profile has six classes and twelve methods; immutable interface
declarations leave two classes and seven methods. Each runtime matrix checks
166 values, 228 identities, 205 states and 60 ordered failures.
