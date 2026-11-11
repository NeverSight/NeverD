# Interface token result and closed exceptions

An inherited interface declares an invariant Class<T> input and its own T
result with a checked exception. A field-loaded receiver forwards that exact
contract without casting through the token or selecting an implementation.

Frozen callers exercise direct, reflective and method-handle calls, future
implementations, opaque/null/mismatched tokens, callback changes, delayed result
casts and exact exception objects. Every original method, access role,
Signature and Exceptions attribute must survive full source compilation.

The `--scoped-default` variant stages a separate original program before compiling and freezing its classes and observer. A generic parent interface supplies the three-argument default body; a generic child interface supplies a two-argument default delegate. The receiver owns the context formal, while each method owns its token/result formal. The independent observer also checks context and boolean identities and mutations, including a separately compiled future override. Source and dependency-provider recovery must preserve every original class/method, Signature, Exceptions attribute and Code role; all generated Java compiles.

Scoped default source recovery uses Java 9 for the existing private interface exception helpers; dependency-provider recovery uses Java 8. Signature controls change the child interface entry actually called by emitted code in both modes, retaining all original Code bytes.
