# Context arguments and producer tokens

The owned provider declares `<T> Box<T> read(Context, String, Class<? extends T>)`.
The complete invariant output slot matches the token's scoped formal, while
ordinary nominal contexts bind no formal. Source, package-source and SDK
profiles preserve original flags, signatures, metadata, exceptions, and frozen
callers. Native cases cover package static nested entries, invariance and
covariance, and reject foreign access and unproven declarations or bounds.

Independent fresh JVM observers compare exact context and token identity,
ordered callbacks, live fields, carrier identity, late element casts, ignored
results, mutations and checked/Error identity. Providers may return a carrier
whose raw contents differ from the token; those contents grant no new binding.
Deliberately wrong programs must fail the observer.

`--protected-provider` compiles a distinct owned variant with a protected static entry. Source and SDK recovery use the same frozen observer and thirteen wrong programs. Native cases also cover standard `Class<T>`/`List<T>` declarations, closed owner/context headers, subclass access, interface callers, unused results and malformed type relations or dispatch frames.
