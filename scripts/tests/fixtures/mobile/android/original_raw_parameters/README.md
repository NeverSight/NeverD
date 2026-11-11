# Original raw parameter subsignatures

The owned nongeneric interface retains `RawBox<?>`, while a captured final
forwarder and an inherited class provider keep their original raw parameter
and Signature-free runtime methods. The whole erased subsignature follows
[Java's method signature rule](https://docs.oracle.com/javase/specs/jls/se8/html/jls-8.html#jls-8.4.2).
The source and unchanged interface-JAR profiles retain all generic metadata,
original method ownership and zero bridges. The immutable JVM harness checks
36 value combinations, 444 identities, 156 mutable state observations and
20 ordered failures through direct, interface and reflected calls. It verifies
captured nulls, alias mutation, virtual overrides, effects before failure and
unchanged raw behavior. Inserting a typed method Signature into either direct
or inherited providers keeps Code and descriptors unchanged, but fails NeverD
and an independent Java override check.

`--dependency-interface --interface-fields` adds two owned constants and one
plain static final state reference to an unchanged dependency JAR. Its immutable
wrapper checks original field ownership and roles, constant values, exactly one
library initialization and mutable aliases. Dependency fields and initialization
are not copied into recovered source. Generic fields and malformed field roles
remain outside this proof.

`--dependency-interface --dependency-parent-chain` instead adds the owned
fixed dependency class prefix. All four SDK classes, generic methods and two
original SDK bridges remain in the unchanged JAR. The forwarder's original
protected override and raw callback retain their metadata; an immutable
wrapper checks SDK ancestry, formal ownership, construction, aliases, virtual
dispatch and ordered failures. A same-Code superclass arity mutation fails
both recovery and javac. See `fixed_dependency_class_prefix` for the bounded
profile and its unsupported shapes.

`--dependency-interface --abstract-dependency-parent` keeps a plain abstract
SDK parent and its deeper root in the unchanged JAR. Original public and
protected providers and an alpha-equivalent method-owned generic override
remain in the input class. An immutable wrapper checks nearest ownership,
reflection, virtual dispatch and ordered failure effects. Changing a plain
protected provider to a method-owned formal preserves Code and descriptors
but fails recovery and javac.

`--dependency-interface --dependency-method-formals` keeps bounded generic
abstract/default methods in a nongeneric unchanged interface JAR. Its own
method variables and the input providers' alpha-equivalent variables retain
their complete bounds, invariant arguments, results and metadata. The wrapper
checks original default ownership, reflected and virtual dispatch, mutable
aliases, captured nulls and ordered failures. A same-Code parameter-scope
mutation fails recovery and independent javac.

`--scoped-nominal-parameters`, optionally with `--dependency-interface`, gives
Api an original class formal and Forwarder/Leaf complete nested wildcard
arguments containing their own class variables. The raw methods remain exactly
as compiled by javac, with no Signature or bridge added. A separate immutable
wrapper checks original formal ownership and 24 additional values,
114 identities, 66 state observations and 12 failures through typed callers,
reflection and MethodHandles. All original metadata and Exceptions stay exact;
same-Code partial typed counterexamples still fail recovery and javac. Native
coverage includes the same relation with captured enclosing method formals.
