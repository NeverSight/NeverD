# Recursive source parents and original unchecked callbacks

The owned `Parent<P extends Parent<P>>` extends `Base<P>` and leaves a plain
`Updates` callback to `Core`. The input child keeps its original ordered
RuntimeException and Error declarations. Parent forwarding and an original
interface default dispatch to that callback without a new parent declaration.

The immutable `RecursiveUncheckedCallbacksHarness.java` checks direct, parent,
interface, default and reflected calls with an independent subclass, recursive
bounds, mutable byte-array aliases, nulls, effects and exact exception identity.
Each profile checks 36 values, 360 identities, 90 states and 54 ordered failures.
Complete original metadata and inventory recompile unchanged apart from reported
private helpers, with zero bridges or metadata projections. The source profile
has seven classes/ten methods; the immutable root-JAR profile six classes/eight
methods. Coverage is `recovered`.

Changing the original exception superclass from RuntimeException to Exception
keeps Code and method descriptors unchanged, but NeverD and independently
authored Java reject the checked override. Original input, immutable harness,
dependency JAR and SDK hashes remain unchanged.
