# Class-owned formals implementing plain Object returns

An abstract `Base<T>` implements plain `SourceView` Object-returning methods
using its own Object-bound `T`. A concrete child specializes `T` as
`List<String>` and inherits both methods without a bridge or redeclaration.
The `echo` method also retains the complete fixed
`Callback<String,byte[]>` parameter contract.

The immutable JVM harness compares direct, parent, interface and reflective
dispatch with different mutable list identities, nulls, callback identity,
side effects and ordered failures. Reflection must retain the original `T`
return and field Signatures, Object descriptors, empty exceptions and absence
of bridges. Source and declaration-JAR forms of `SourceView` are checked.

Metadata-only method-formal shadowing and swapped callback arguments must fail
without changing Code. Independent javac counterexamples check those illegal
overrides. Different values or another valid class-owned Object-bound formal
are not treated as invalid overrides.
