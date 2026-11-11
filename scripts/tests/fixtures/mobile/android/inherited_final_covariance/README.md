# Bridges to inherited final covariant getters

Core inherits Provider's public final narrow getter and implements an interface
requiring a wider generic result. Its only declared getter is the original javac
bridge. Complete superclass and interface arguments must agree; copying a typed
method into Core would change the original runtime inventory.

Run `test_mobile_android_inherited_final_covariance.py`, optionally with
`--dependency-provider` to preserve the original Provider, Api, Broad and Narrow
classfiles in a declaration JAR. The independent JVM harness compares original
and rebuilt direct, parent, interface, reflected and MethodHandle calls through
Core and an independently compiled subclass. Nullable mutable results, effects,
exception identity and all original metadata remain checked.

Same-Code flag and generic-interface-binding mutations are rejected, and javac
independently rejects the conflicting binding. Native controls cover original
virtual and super calls, casts, source/SDK declarations, ordering, scopes,
receiver provenance and the required final declaration. Rebuilding regenerates
only the original child bridge; its compiler details keep coverage `partial`.

The source profile retains six classes and ten original methods; the SDK
profile retains two input classes and five original methods. The immutable
oracle compares 96 values, 672 identities, 288 state observations and 144 ordered
failures; no original metadata change or typed child method is introduced.
