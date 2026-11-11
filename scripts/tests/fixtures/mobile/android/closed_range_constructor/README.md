# Closed recursive bounds in default constructors

Owned Kotlin retains a Range<Float> whose T has the recursive interface bound
Comparable<? super T>. Its synthetic constructor selects a default integer and
delegates to the same original instance. The closed Float argument is already
checked by the signature resolver; no new type formal or range element cast is
needed to emit that delegation.

The frozen observer covers seven masks, two marker identities, float raw bits,
raw and typed ranges, deferred reads and casts, nulls, exact checked and unchecked
failures, and fields visible on a partially constructed instance. Ten wrong
programs must fail before NeverD runs. Source and SDK-declaration profiles compile
all emitted Java and compare every original definition, complete Signature and
Exceptions. Only disclosed synthetic flags, source filenames, CLASS annotations
discarded by D8, class-file versions for the required Java 25 constructor
prologue, and reported source helpers may differ.
