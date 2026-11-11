# Nearest plain class contracts and original clone covariance

This independently authored class extends the SDK's parameterized map hierarchy
and overrides clone covariantly. Javac emits its original erased forwarding
bridge. The nearer SDK declaration is public and has no checked exceptions;
a farther protected ancestor declares a checked exception. Each original input
bridge still needs its own complete body proof.

The immutable JVM harness uses direct, inherited and reflected calls plus an
independent subclass. It checks shallow payload aliases, separate map storage,
insertion order, mutable trace fields, exception identity and null receivers.
Full compiler metadata and runtime inventory stay exact apart from reported
private helpers. Only the original bridge is regenerated and reported partial.
A Same-Code bridge-flag removal fails recovery; an independently authored
return-only overload fails javac. Native controls include a nearer abstract
contract, missing bridges, changed results, static/final slots, unknown scopes
and exception drift.
