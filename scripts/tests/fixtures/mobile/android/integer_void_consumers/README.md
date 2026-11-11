# Integer consumers with void targets

An owned final nonstatic member implements `Action<Integer>` through an original
Object bridge that casts to `Number`, calls `intValue()` and invokes the original
`accept(int)` void target. The runner encodes only the raw class Signature and
bridge flags after compilation. Original Code, descriptors, names, capture and
mutable outer state remain unchanged.

The immutable JVM harness compares primitive, Object, interface and reflective
calls. It checks integer edges, other Number types, one virtual conversion,
conversion failures, target failures, wrong types, nulls and captured effects.
Raw interface metadata, bridge flags and the existing static member capture
projection are explicitly measured. All original members and other metadata
must match; same-Code incompatible full interface arguments must fail.

Run with a JDK supporting Java 25 because the established captured member source
projection needs flexible constructor bodies. `--dependency-interface` exercises
an unchanged declaration-JAR `Action` interface.

`--inherited-interface` makes `Action<T>` redeclare the method of an immutable
declaration-JAR `RootAction<T>`. The additional harness checks root interface and
reflective dispatch, conversion and target failure order, and both complete
interface parameter scopes. A same-Code parent edge changed from `T` to `String`
must fail for source and dependency forms of `Action`; an independent javac
counterexample checks the same conflicting inheritance.
