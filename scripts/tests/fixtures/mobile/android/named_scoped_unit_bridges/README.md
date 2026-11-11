# Named scoped void/Unit callbacks

Core<E> extends Parent<E> and implements a complete three-argument generic
BiTask contract. Its original apply(Object,Object) bridge checks the second
argument, invokes apply(E,Throwable):void, then reads Unit.INSTANCE. The runner
starts with independently authored Java and changes only the callback name,
bridge flags and complete class Signature. Every original Code attribute stays
unchanged. The source rename avoids an original method inherited from Parent.

Run `test_mobile_android_named_scoped_unit_bridges.py`, optionally with
`--dependency-parents` to keep both Parent and BiTask in an immutable JAR. An
immutable harness uses seven call routes, full generic reflection, mutable
aliases, nulls, ordered casts and exceptions. Separate fresh JVMs exercise Unit
initialization failure and subsequent erroneous-class accesses through every
bridge route. Ordinary typed void callers never initialize Unit, including
after the failed initialization. A helper performs the deliberately failing
initialization before the singleton's straight-line field assignment.

The report explicitly maps the original void method to its source name and
records the added Unit wrapper. The original erased bridge is regenerated.
Complete original class, field, method and Exceptions metadata are compared
under that name mapping; no other changes are accepted. Original input,
harness, SDK and dependency bytes are hashed and preserved. Same-Code bridge
flags and method-owned scope counterexamples fail recovery; independent javac
rejects the conflicting method-owned formal. Each profile checks 14 values,
383 identities, 290 state observations and 85 ordered failures. The source and
parent-JAR profiles retain five classes/eleven methods and three classes/eight
methods respectively. Native controls
also reject inherited void slots, nonfinal owners, Object member names and
incorrect forwarding registers or read order. Coverage remains `partial`.
