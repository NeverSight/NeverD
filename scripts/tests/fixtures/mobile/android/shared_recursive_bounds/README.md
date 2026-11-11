# Shared recursive method bound fixture

These independently authored Java sources capture a method's
`K extends Comparable<? super K>` alongside a selector. The runner removes only
the named Worker's class formals and places its enclosure in the exact generic
`Owner.make` method; both InnerClasses tables are kept consistent. All original
Code attributes and runtime descriptors must remain unchanged.

The D8/JVM oracle compares raw interface and typed covariant dispatch, distinct
Comparable implementations, mutable selection and ordering, null paths, bridge
casts, callback and comparison sentinel failures. Owner and comparison method
bounds and the Worker's independent shadowing method formal remain unchanged.
The source uses `Comparable<?>` for captured K, explicitly reporting lost
self-recursive generic reflection and variable identity. It requires all source
type arguments to be checked again; dependent recursive constraints still fail.
No private artifact is included.
