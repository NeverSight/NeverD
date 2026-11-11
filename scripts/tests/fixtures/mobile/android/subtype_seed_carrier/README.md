# Dependent input and seeded result carrier

The owned Kotlin provider has `<T : R, R>` with a producer of `T`, a direct
`R` seed and an invariant result carrier of `R`. Its genuine compiler Signature
encodes the sole variable bound in the interface-bound slot. Java recovery
reports only the equivalent bound-position spelling change.

The frozen Java observer checks complete reflection contracts, four caller
routes, opaque input/seed/result identities, changing producer/context/result
state, partial effects, nulls, wide flags and exact RuntimeException/Error
identity. Every case runs twice in a fresh JVM. The result is never evaluated.
Source and SDK-provider profiles compile every generated Java and compare all
original definitions, metadata and Exceptions. Eight wrong providers must fail
the same observer. The source-only facade's disclosed constructor is checked
as private and nongeneric; it is absent from the original Kotlin class. No
private sample data is included.
