# Dependent class bounds

The input subclass declares `U extends T`, keeps its original generic superclass
and interface arguments, and inherits a method with its own dependent formals.
The optional owned class-file mutation places only U's bound in the interface
slot. Code bytes, runtime descriptors and the original bound relation remain
unchanged. Both class encodings are tested with input or immutable JAR providers.

The immutable JVM harness and caller check 352 identities, 176 states and 24
failures, including the dependent class bound's exact reflective declaration,
method bounds, arrays, callbacks, aliases and exception identity. All class,
field, method and exception metadata are compared. Only the reported class-bound
position and private helpers may differ. Input inventories remain four classes/
six methods or one subclass/constructor with dependency providers.

Same-Code negative inputs change a method's class-formal bound, its dependent
method bound, or the superclass argument to a different formal with equal
erasure. Both input and dependency variants must reject those changed contracts.
Independent javac rejects an incompatible inherited method. Native tests also
combine class and method bound-position encodings and reject undeclared,
self-referential and cyclic class bounds.
