# Array factories through generic superclasses

Run `test_mobile_android_boxed_array_superclass_results.py`, optionally with
`--dependency-parent` for immutable Base/Parent/Api declarations.
The owned source calls an original byte[] target followed by Box.box(byte[]).
Before input hashes are frozen, only Core's Parent<Object> Signature is encoded
as Parent<Box> and its original get() flags become public/bridge/synthetic.
Every descriptor and Code attribute remains unchanged. Recovery retains both
calls and discloses its Object source argument and original bridge flag change.

The independent oracle compares six class/interface/reflective/method-handle
routes, original mutable array identities, nulls, new/cached/null factory results,
target/factory order and original exception identity. Typed target calls must
not add a factory invocation. Recompiled original metadata and Exceptions match
outside the two disclosed differences and reported private compiler helpers.

Native cases cover zero/indexed arguments, ten array descriptors, ancestor
interfaces, source/dependency declarations, ordering and malformed result kinds,
receiver origins, factory arguments, returned origins, scopes and flags.
Same-Code superclass-argument and bridge-flag counterexamples are rejected;
independent javac rejects an incompatible generic return.

The matrix checks 171 values, 831 identities, 171 states and 114 failures.
The source profile contains seven classes and fifteen methods; the dependency
profile retains four classes and seven original methods. Coverage is partial.
