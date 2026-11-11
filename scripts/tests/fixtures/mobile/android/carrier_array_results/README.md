# Invariant carrier array results

Original javac/D8 output calls an inherited generic interface method with two
integer arguments and an invariant Class<T> input, returning T[]. The provider
keeps its arbitrary original array and side effects. An independent frozen
caller observes the later String[] cast, so an inferred component cast, copied
array or altered provider cannot pass.

Four dispatch routes and 18 modes repeat calls against nulls, distinct array
runtime types, extreme integer inputs, primitive/different Class tokens,
callbacks, partial array stores and exact exception identity/order. Owned and
dependency interfaces preserve every original definition and metadata field;
all recovered Java must compile. Deliberate wrong providers and earlier casts
must fail the unchanged observer. Native checks also cover generic carriers,
static/virtual/interface calls, wide values, range encodings and scoped drift.

The deferred-provider profiles leave the original requirement on an abstract
generic parent, redeclare it with a fresh method formal on an abstract middle
class, and keep the actual body on the original concrete provider. No methods
may be copied or invented. Both source/dependency interface paths use the same
frozen 72-case observer and complete inventory/metadata comparison.
