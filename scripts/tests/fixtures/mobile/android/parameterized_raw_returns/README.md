# Parameterized results beneath an original raw superclass result

The inherited-generic-empty-throws oracle adds an owned protected generic
parent method and a child instantiated with raw `AtomicReference`. The child's
original typed method returns `AtomicReference<?>`; javac supplies a protected
erased bridge. Both original class and method Signatures remain unchanged.

Direct and reflective typed, parent and bridge calls preserve returned object
identity, mutable AtomicReference payloads, null results, ordered field effects,
the original exception object and null-receiver failure. An unchanged-Code
parent argument mutation to `AtomicReference<String>` must fail complete result
comparison, and independent javac must reject the same incompatible override.
