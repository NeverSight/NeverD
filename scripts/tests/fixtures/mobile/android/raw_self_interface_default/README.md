# Raw self-parameter defaults and original checked bridges

The authored interface retains a scoped Comparable<Self<T>> parent while its
original default takes a raw Self parameter. Javac emits the checked Object
bridge that invokes this exact default through the interface. The original
whole raw subsignature supplies no method Signature, and its full class and
parent Signatures remain unchanged.

An immutable JVM harness checks direct, Comparable and reflected calls,
independent overrides, mutable aliases, ordered rank effects, exception identity,
wrong erased arguments and null receivers. Full metadata and runtime inventory
remain exact apart from reported private helpers and one regenerated original
bridge. A partial Self<String> method Signature stays outside the whole raw
subsignature proof. Removing only original bridge flags preserves Code and
runtime descriptors but fails NeverD; an independent explicit Object default
fails javac. Native controls
retain scope, invocation, flags, cast and register-provenance requirements and
reject missing bridges, abstract targets and inherited method formals.
Coverage remains partial.
