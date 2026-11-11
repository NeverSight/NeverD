# Closed anonymous guards and original superclass adapters

An instance factory returns an anonymous generic subclass that captures its
original host. The two erased guards preserve false/-1 for wrong and null inputs;
the two typed entries call the original superclass implementations. Metadata
encoding adds only final class/InnerClasses flags and four original bridge flags.
All Code, Signatures and descriptors remain unchanged.

The immutable harness compares 102 values, 436 identities, 490 states and 20
failures through five erased call routes and direct reflected typed entries.
It checks parent effects before callback failures, sibling and separate host
identities, null behavior and the preserved explicit capture. Reflection also
constructs the rebuilt named local with different implicit/explicit host
arguments and proves the explicit original capture remains authoritative.

Inventory comparison permits only reported local name/constructor mappings and
private source helpers. Generic scopes and other fields/methods remain unchanged.
The existing report discloses the named local, four bridge flag differences,
capture-field synthetic flag and extra implicit enclosing constructor parameter.
The parent-JAR profile keeps the original dependency bytes unchanged.

Same-Code removal of a guard's final flag and an independently compiled adapter
with an additional state change must fail. Run the corresponding Python runner
with JDK 25 or newer for the original pre-super capture assignment, optionally
using `--dependency-root`.

`--nullable` authors the original guard with a shared condition: null supplies
true, while nonnull inputs use `instanceof String`. Both arms join the checked
forwarding branch. The same immutable-harness observation counts now include
null's superclass and captured-host effects, including construction with distinct
implicit and explicit host arguments. This variant also supports
`--dependency-root`; unrelated wrong types retain false/-1 and no effects.
