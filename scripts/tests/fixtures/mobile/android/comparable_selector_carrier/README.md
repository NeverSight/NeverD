# Comparable selector and invariant element carrier

The owned method has one Object-bounded element formal and an independent key
formal with its original recursive Comparable<? super K> bound. Its input and
result are invariant Box<T> carriers; the callback consumes T and produces K.
Provider behavior is independently authored: it captures the input, calls the
selector, compares its actual key and returns a configured carrier. This proves
call binding without predicting any sorting algorithm or returned-input identity.

Frozen direct/separate/reflection/method-handle callers cover 68 fresh JVM cases
with two attempts, class initialization and failed initialization caching,
inherited static facade lookup, null and non-Comparable keys, other Comparable
implementations, input/result snapshots, original references and exact failures.
All generated Java compiles, and original definitions, complete recursive
generic metadata and Exceptions must remain unchanged.
# Boolean result profile

`--void-result` freezes the same input/key relationship with an original void
entry and an observable write through the earlier result snapshot. It checks
all callback/comparison effects and failures without inferring a return value.
The immutable observer rejects missing, redirected and changed writes.

`--boolean-result` independently authors and freezes a primitive boolean entry
with the same complete element/selector/recursive key relation. The observer
checks true/false results, original comparison and cast timing, mutable input
and result snapshots, failures and cold inherited initialization. Six same-code
signature mutations and eight separately compiled wrong programs must fail.
