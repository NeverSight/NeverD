# Two independently seeded formals in a static void call

This owned fixture has two separate Object-bounded method formals, a nongeneric
state input, scalar indices, and an optional wide scalar. Frozen callers use the
original erased descriptors; reflection checks both complete scoped formals.

The operation callback changes the state and backing array. The observer checks
the earlier size snapshot, later array snapshot, store order, alias identity,
partial writes, null/index/store failures, original RuntimeException/Error
identity, inherited static initialization, and full-width scalar bits.

Source and dependency-provider profiles independently inventory every original
definition, preserve metadata and Exceptions, compile every generated Java file,
and execute only this owned fixture. Signature mutations and separately compiled
wrong implementations must fail the unchanged observer.
