# Contained consumers across generic inherited contracts

Root uses a closed lower wildcard consumer. Middle redeclares that runtime slot,
Leaf inherits it, Core provides the original delegated implementation through a
generic superclass and repeated interface path, and FinalCore inherits Core's
body. The runner compiles an immutable harness first, then changes only the
Middle/Core output parameter Signature to an unbounded wildcard. Original Code,
descriptors, class scopes, fields and runtime entries remain unchanged.

Run `test_mobile_android_inherited_contained_consumers.py`, optionally with
`--dependency-root` to keep Root, Consumer, Value and Base in an immutable JAR.
The independent provider and overriding subclass remain outside the input DEX.
Eight routes check original virtual dispatch, aliases, callback mutations, exact
exceptions, null receivers/delegates and wrong-callback failures after the
original preceding effects. Complete metadata and Exceptions permit only the
two explicitly reported source method Signatures to change. No inherited method
is copied into FinalCore, and no bridge is added.

Same-Code partial input, upper output and method-formal counterexamples are
rejected. Independent javac rejects the unprojected wildcard redeclaration.
Native controls also cover class/member order, repeated and transitive paths,
source/SDK roots, open or unknown scope, changed access, conflicting contracts
and immutable dependency redeclarations. Coverage remains `partial`.

The source profile retains eight classes and eight original methods; the SDK
profile retains four classes and four original methods. The oracle
compares 144 values, 264 identities, 416 state observations and 80 failures.
Only two method Signatures change through the reported projections.
