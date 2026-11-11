# Inherited checked result bridges

This independently authored Java fixture has a generic concrete `Parent<T>`
implementing `Generic<T>`, plus a fixed `Api` returning `Value`. An otherwise empty `Core` and static nested
class inherit `Parent<Value>` and implement `Api`. Javac supplies their original
narrow result bridges through superclass calls. No classfile edits create the
positive case.

The runner compiles the original classes and separate frozen callers, checks the
DEX inventory and all six four-operation bridge bodies, recovers every source
class, compiles all output, and compares the exact original runtime method,
class, field, generic and Exceptions metadata. Expected throw helpers are listed
independently. Source and dependency-provider profiles cover the same contracts.

The JVM matrix exercises zero, integer and mixed wide/reference parameters,
normal and null results, mutable identities, raw NaN bits, provider exceptions
before result casts, direct/interface/reflection/method-handle calls, recovered
and frozen source callers, and external subclasses. Heap-polluted parent results
must remain accessible through the inherited broad runtime entry while the
narrow bridge throws after the parent's effects. Adding a source override would
invent an Object-return bridge and fail both inventory and behavioral checks.

Missing synthetic flags and incompatible superclass arguments are rejected with
unchanged original Code. Independent javac rejects the incompatible source
contract. Native tests also reject wrong receiver/parameter words, dispatch,
cast/result provenance, extra payload, competing declarations and metadata.

Each profile checks 188 values, 228 identities, 263 state observations and 114
ordered failures. The source profile retains eight classes and 26 methods;
the dependency profile retains five input classes and 16 methods with the three
original provider/interface classes supplied unchanged by its owned JAR.
