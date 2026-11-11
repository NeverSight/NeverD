# Original Comparable carrier predicates and factories

This independently authored fixture implements two raw recursive interfaces and
an inherited interface scope. Encoding changes only the original class arguments
to `Carrier` and the three erased implementation flags to bridge/synthetic.
Code and all runtime descriptors are unchanged. The scalar predicate targets
have different names; the original getter invokes a scalar target and then a
carrier factory. No specialized carrier entry exists in the original class.

The immutable harness compares eight direct, interface, reflection, MethodHandle
and original caller routes. Its 96 value, 346 identity, 624 state and 88 failure
observations preserve ordered conversions, wide words, aliases, parent effects,
wrong and null inputs, exact exceptions, factory effects and fresh results.
Full metadata comparison permits only the two reported raw interface edges and
three original bridge flag differences. The carrier's ordinary compiler bridge
is regenerated when the carrier is an input class.

`--dependency-root` leaves the interface, parent and carrier JAR unchanged.
`--narrow` mechanically substitutes int for long in the authored sources and
immutable harness before compilation. Both widths use the same original-body
oracle. Same-Code wrong interface arguments and missing synthetic flags must
fail NeverD, and an independent source counterexample must fail javac.

`--inlined-carrier` compiles an original checked `compareTo(Object)` body and
encodes only its bridge flags and original `Comparable<Carrier>` class argument.
The complete carrier source projection must compose with the scalar conversion
proof, using its saved original contract. The carrier remains an input even in
ancestor-JAR mode. Additional direct, reflected and MethodHandle comparisons
raise the oracle to 105 values, 347 identities, 630 states and 94 failures.
