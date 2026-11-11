# A nested instance factory returning its private enclosing carrier

Owned Java has a public static Factory nested inside a private static Cell<E>.
Its public final instance method introduces U independently of E, transfers
Source<U> to Cell<U>, and keeps the original declarations and erased descriptors.
The containing Core supplies the exact private-member lexical access anchor.

Frozen observers inspect the original receiver, input snapshots, getter calls,
fresh allocation, partial final fields, callbacks, actual replacement/null
results, result casts and exact failures. Source and SDK input declarations run
the same bytecode and observers. All emitted Java is compiled and complete
metadata and Exceptions are compared except reported source helpers; input and
observer hashes remain frozen.
