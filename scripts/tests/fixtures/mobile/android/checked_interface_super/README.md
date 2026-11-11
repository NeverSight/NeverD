# Checked interface arguments to a generic superclass constructor

An independently authored `Child<E>` constructor accepts `Set<? extends E>`,
performs an explicit erased `Collection` check, and invokes the exact
`Parent<E>(Collection<? extends E>)` overload. The parent also has `Set` and
`Object` overloads. No collection or element operation is part of this call.

The frozen observer covers direct, frozen-caller, reflection and method-handle
construction in fresh JVMs, each with a second attempt after initialization.
It checks reference identity, null and polluted sets, replacement of the global
input during delegation, partial final-field writes, initialization order and
the exact original RuntimeException/Error at each constructor step. Spy sets
reject any inserted collection query. All source metadata, original definitions,
Exceptions and generated helper accounting are checked separately.

Source and dependency profiles share identical original classes and observers.
Same-Code signature mutations reject incomplete or mismatched carrier relations.
Independent wrong programs must fail the frozen observer. Native cases cover
complete interface chains, invariant/covariant slots, copied/range frames,
declaration order and incomplete graphs.

The original Java 8 input and frozen observers remain unchanged. Recovered
source requires Java 25 to materialize the original cast before delegation.
