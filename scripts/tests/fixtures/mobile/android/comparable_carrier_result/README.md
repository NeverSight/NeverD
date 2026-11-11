# Recursive carrier inputs with Comparable result erasure

A complete Collection<T> input binds the method-owned Comparable<? super T>
formal while the opaque result retains its original Comparable erasure.
Four frozen invocation paths run 192 fresh cases twice, checking input and
result identity, null inputs, absent traversal, callbacks and partial effects,
original bounded casts, delayed concrete casts and deferred comparisons.
Fifteen wrong programs and eight same-Code Signature controls qualify the
observers. Source and SDK paths compile every emitted source and preserve
original definitions, roles, Signature and Exceptions attributes.
