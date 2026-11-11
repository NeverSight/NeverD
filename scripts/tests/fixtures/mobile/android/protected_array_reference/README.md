# Protected array inputs with reference results

A final Core implements Base<boolean[], Result> with a protected array parameter
and a concrete reference result. Its original Object-to-Object compiler bridge
is encoded as public, changing no Code attribute. Recovery retains that entry
and the complete generic parent arguments, while reporting only the typed
method's source visibility change.

The immutable caller and harness compare six inherited, typed, reflected and
method-handle routes. They observe the exact returned Result, its retained input
array, the receiver's last-result alias and constructor side effects. Null
receivers, null arrays, original target failures and constructor failures keep
their ordering. Wrong primitive array elements, dimensions and plain objects
fail before the target or constructor runs.

The original/rebuilt class, method, field and Exceptions inventories must match
outside the reported access change and private static helpers. Base also runs as
an immutable dependency. Same-Code controls change either parent argument or
remove the synthetic flag; independent javac rejects a wrong array type. Native
cases cover all eight primitive arrays, reference/multidimensional arrays,
source order, copied casts and altered input/result register origins.

The source profile has three classes and seven methods; the dependency profile
has two classes and four methods. The JVM matrix checks 65 values, 125 identities,
117 states and 69 ordered failures.
