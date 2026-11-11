# Protected generic array void calls

This authored fixture has a nongeneric abstract owner with a concrete protected
`<T> void consume(T[])` loop. Its public generic wrapper keeps a scalar prefix
before the original call on `this`. A subclass has the same forwarding shape,
so the SDK-provider profile still contains a recovered protected call after
the base declaration moves out of the DEX. A further subclass overrides the
callee with distinct observable phases.

The observer reuses each receiver across two attempts and checks array aliases,
prefix and loop ordering, prior state after abrupt exits, callback mutations,
per-element reads, partial writes and virtual dispatch. Inputs include null,
empty, Object, String and CharSequence arrays; compatible and incompatible
stores distinguish runtime ArrayStoreException from callback exceptions. The
original RuntimeException and Error instances must propagate unchanged.

Every emitted Java unit and original definition is checked in both source and
SDK profiles. No metadata projection is expected. Wrong source programs test
the independent observer, and invalid Signature controls preserve all Code
bytes while changing formal, bound, array-rank or return erasure metadata.
