# Complete generic input and carrier conversion

This independent fixture checks Box<S> and CarrierA references, calls the
original virtual Number.intValue conversion and forwards those unchanged
references to apply(Box<S>, CarrierA, int) -> Box<S>. Its raw Function class
Signature and original invoke flags are the only owned input metadata edits;
all Code attributes, descriptors and the complete target Signature remain.

The JVM oracle checks nullable references, exact object identity, scoped target
reflection, integer edge values, non-Integer numbers and an effectful Number
override, as well as ordered cast, unboxing and target failures. The target
Signature counterexample changes only the first Box<S> to Box<V>, keeping the
same runtime erasure and Code. Recovery must reject it. Raw-interface and
bridge flag differences in source remain explicit.
