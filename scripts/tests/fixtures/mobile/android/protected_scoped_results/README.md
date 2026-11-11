# Protected scoped generic results

A final class retains its class formal through four protected methods: a getter,
a scalar callback, an alias-preserving reference method and a reference method
with an upper-wildcard parameter. The original input encoding changes only the
four compiler bridges from protected to public. Code attributes are unchanged.

Recovery retains full class and method Signatures, runtime descriptors and
original bridges, and reports each typed target's public source access. The
independent harness and package caller exercise inherited, typed, reflective and
method-handle routes, null receivers and values, extreme scalar results, original
failures, invalid erased inputs and mutable aliases. Full metadata and exception
declarations are compared outside the four disclosed target-access changes.

The source profile retains three classes and nineteen methods. Immutable Base
and Box declarations leave one input class and nine methods. Same-Code controls
change an invariant container argument, introduce a method-owned return scope
or remove a synthetic bridge flag. Independent javac rejects an incompatible
superclass argument. Native controls also cover nonfinal owners, private targets,
result/parameter scope and a corrupted returned register.

The JVM matrix checks 596 values, 850 identities, 364 states and 226 ordered
failures, including original public bridge flags after recompilation.
