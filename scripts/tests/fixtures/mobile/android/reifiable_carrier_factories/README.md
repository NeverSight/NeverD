# Reifiable input with an independent result formal

Authored `fixture.*` code exercises `<T> List<T> make(Collection<?>)` through
an inherited public facade whose declaring provider has package access.
The unbounded input binds no method formal. The result retains the complete
invariant formal and its Object bound without predicting contents or identity.

The frozen caller runs 96 fresh JVM cases twice, using direct, separately
compiled, reflection and method-handle calls. It checks original collection
identity captured before class initialization and provider callbacks, null and
poison collections, partial mutation, actual mutable/immutable results, late
String element casts, initialization failures and exact exception identity.
Provider and caller field changes cannot replace the captured argument.

Source and dependency profiles compile every generated Java definition and
compare all original declarations, metadata, Exceptions and generated helper
roles. Signature-only counterexamples keep method Code unchanged. Independent
wrong programs bypass or repeat the provider, substitute/copy/inspect the input
or read it late, and must fail the unchanged JVM oracle. All fixtures are
independently authored; no private application code or identifiers are used.
