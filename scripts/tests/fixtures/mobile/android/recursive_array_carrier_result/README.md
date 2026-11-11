# Recursive array carrier result

Independently authored Java 8 keeps a first-rank array and an invariant list result
under the same `T extends Comparable<? super T>` method contract. A public facade
inherits its static provider from a package-private class. The provider publishes
the original array, invokes an external observer and returns a separately supplied
list; no sorting, traversal, copying or selection is implied by the signature.

Four frozen caller routes compare raw returns and later checked element reads.
Source and SDK layouts compile every emitted unit and compare complete original
class/method/Code/access/Signature/Exceptions/MethodParameters metadata. Cases
cover null, empty and covariant arrays, callback array stores, opaque and null
results, original casts, partial effects and exact runtime/Error identity. Wrong
programs and Code-preserving Signature controls exercise behavior and binding
independently. These fixtures contain no private application code or identifying
information.
