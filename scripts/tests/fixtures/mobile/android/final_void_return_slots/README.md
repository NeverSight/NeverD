# Final void JVM return slot beside a generic scalar provider

The authored `Core<T>` extends `Middle<T>` and `Base<T>`. The original public
final void entry and the original inherited public boolean entry both use the
name `add` and the same Object parameter descriptor. Their complete generic
parameters bind to the same original class formal. The class-file encoder only
renames the authored `append` entry and its original caller references; every
Code byte, descriptor, flag and Signature stays unchanged.

The source planner records `incompatible-inherited-return-slot` after composing
the actual nearest class-provider path and comparing complete parameters.
Only the final void method's source name changes. Original caller bodies,
parent ownership, generic class/method Signatures and both runtime bodies stay
intact. Rebuilt runtime name and method-name reflection differ as reported.

The immutable harness compares six void routes and seven scalar routes,
including original callers, reflection, MethodHandles and the unchanged
interface default. It verifies 56 values, 130 identities, 403 states and
26 failures: repeated mutation, aliases, null receivers and exact exception
identity retain their original order. Complete metadata and inventory allow
only the disclosed source name and reported private helpers. Source and
immutable ancestor-JAR profiles retain five classes/11 methods and two
classes/six methods.

Wrong complete ancestor binding and missing final entry counterexamples keep
original Code and descriptors but fail recovery. An independently authored
incompatible void override fails javac. Native controls also reject missing
roles, open method formals, wrong variable scope, invalid visibility, static or
bridge declarations and a missing concrete inherited provider.
