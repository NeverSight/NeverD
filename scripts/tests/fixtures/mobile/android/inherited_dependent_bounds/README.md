# Inherited dependent method bounds

A subclass inherits the complete implementation from a generic parent and
implements a separate generic interface. The parent's first method formal is
bounded by its class formal, and its second formal is bounded by the first.
Both bounds must match after composing the subclass's superclass arguments;
array and callback arguments alone cannot establish that relationship.

Ordinary and single interface-slot bound encodings are tested with input or
immutable dependency providers. The independently compiled harness and caller
check 352 identities, 176 states and 24 ordered failures. Original arrays,
callback ordering, exception identity, full metadata and generic reflection
stay intact except disclosed source bound-position spelling. The source profile
retains four classes/six methods; the dependency profile recovers only the input
subclass and constructor. Neither copies inherited methods into that subclass.

Same-Code counterexamples change the first class-formal bound or the second
dependent bound, including mutations in a separate negative dependency JAR.
Independent javac compilation also rejects the incompatible inherited method.
Native controls cover separately sourced interfaces/providers, class order,
scope substitution, widening a bound, access and cycles.
