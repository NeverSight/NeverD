# Raw interface arguments from original captured scopes

This independently authored member comparator owns an unused Object-bound
formal and captures its generic outer instance. Its original compare parameters
name the outer class formal. The runner changes only its interface Signature to
raw, preserving Code and runtime descriptors. Recovery must infer the interface
argument from the exact original lexical binding, then let the existing captured
scope projection disclose its Object erasure and static-member metadata changes.

An immutable JVM harness checks original direct, interface, reflected and SDK
default calls, outer-instance identity, mutable outer and member fields, integer
results, effects before failure and exception identity. Full compiler metadata
permits only exact disclosed changes; every original runtime entry is preserved.
Same-Code scope shadowing and parameter conflicts must fail recovery and
independent javac. Rebuilt constructors require JDK 25 for pre-super captures.
