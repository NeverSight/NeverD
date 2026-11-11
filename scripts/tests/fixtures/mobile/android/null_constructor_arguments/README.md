# Null generic constructor arguments

Owned source and dependency-provider profiles retain the exact `this`/`super` target, full `Sink<String>` parameter, proven null argument, mutable state, initialization order and original exception identity. Every emitted Java source compiles, and an independent classfile inventory compares the original entries, access flags, generic signatures and Exceptions attributes.

`--closed-throws` compiles a separate owned variant whose generic target and callers declare `IOException`. Checked failures occur before and after the target's field stores; initialization and caller failures remain runtime exceptions. The frozen direct, separately compiled, reflective and method-handle callers exercise 64 cases twice. Six independent wrong programs change the target, null argument, effects or checked exception identity. Native tests also cover nested closed contexts, class scopes, source/SDK declarations, exception hierarchies and invalid null origins.
