# Complete specialized covariance and original erased entries

The authored interface has a class-owned result and an invariant parameterized
carrier. Its input implementation fixes that result to Number but returns Byte.
Javac emits the original Object bridge; no Number runtime method exists. Source
and immutable interface-JAR profiles retain all compiler metadata and runtime
entries apart from reported private helpers and regenerate only that bridge.

The immutable JVM harness checks typed, raw and reflected calls, independent
subclass overrides, mutable fields and carrier aliases, SDK Number dispatch,
null receivers, exception identity and field casts after target effects.
A Signature-only permutation of full carrier arguments retains Code and runtime
descriptors but fails NeverD and independent javac. Native controls require the
same complete covariance receipt and exact original bridge, including flags,
receiver and parameter origins, invocation kind, return register, original
method scopes and known result ancestry. Coverage remains partial.
