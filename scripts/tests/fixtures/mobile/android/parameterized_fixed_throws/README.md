# Closed fixed exceptions on complete parameterized bridge contracts

Two owned classes implement the same interface instantiated with
`Collection<?>`. One declares both original checked exceptions on its typed
method; the other typed method has an empty exception list. Both original
erased bridges retain the interface's ordered Exceptions attribute.

The JVM oracle checks typed, generic, raw and reflective dispatch, mutable
collection aliases, checked and unchecked exception identity, ordered field
effects, null inputs/results/receivers, and bridge casts before target effects.
Complete original metadata and method inventory must survive recompilation.
Same-Code parameter/result and exception mutations and independent javac
incompatible type/checked-exception overrides must remain rejected.
