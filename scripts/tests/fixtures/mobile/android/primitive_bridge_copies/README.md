# Original primitive argument copies in package bridges

The owned `Parent<T>` has four package abstract members accepting an Object,
its class formal and int/float/long/double arguments. `Core<G extends CharSequence>`
retains its full class-owned scope and all four original erased package bridges.
D8 disassembly must witness scalar and wide moves and an invoke-virtual/range
in every original bridge.

The immutable package-local `PrimitiveBridgeCopiesHarness.java` checks direct,
typed/raw parent and reflected typed/bridge/parent routes with an independent
override. It compares 720 values, 3,648 identities, 912 states and 192 failures:
integer extremes, raw NaN payloads, negative zero, infinities, mutable aliases,
original exception objects, and casts before target effects remain unchanged.
Source and immutable parent-JAR profiles keep two classes/fourteen methods and
one class/nine methods, full metadata and runtime entries apart from reported
private helpers. Partial coverage discloses only four original compiler bridges.

A method-owned formal mutation with unchanged Code and runtime descriptors
fails NeverD and independent javac. Native profiles cover all move encodings,
class/interface source/SDK roots, public/package roles, overlapping wide copies,
wrong/high-word/uninitialized origins, changed argument order and malformed
wide lanes. Original input, harness, dependency and SDK hashes stay intact.
