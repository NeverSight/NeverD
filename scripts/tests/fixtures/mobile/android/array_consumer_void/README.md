The owned array consumer fixture retains `<T>(T[], Sink<? super T>, int,
int)void` through a raw caller and an inherited static reference. Both complete
source definitions and frozen provider/facade/consumer dependencies are checked.
Five routes (source, frozen source caller, frozen provider caller, reflection and
method handles) each run23 cases in fresh JVMs, with two attempts per case.

The immutable observer checks independent array snapshots, comparator inputs,
callback and static-initialization timing, signed decisions, original array and
replacement identity, overlapping indices, covariance and partial writes.
Callbacks replace external array/callback/replacement references and mutate the
original array. Nulls, invalid indices, original RuntimeException/Error objects,
ArrayStoreException, first/repeated initialization failures and failures before
or after each write remain observable. Full method reflection retains the own
Object formal, its array component and the lower-bounded consumer relation.

All generated Java compiles; independent SDK and compiler inventories, access,
Signatures, fields, class relationships and Exceptions must match. Eight
unchanged-Code Signature mutants must fail recovery; nine separately compiled
wrong programs must fail the frozen observer. Original classes, DEX and observers
are hashed before recovery and compared again after verification.

The `--boolean-result` profile authors a separate boolean provider, clients and
frozen observers before compilation. It retains both actual true/false results
through all five routes, alongside the same mutation and failure observations.
Reflection requires primitive boolean; three additional wrong implementations
(reverse the comparison, always true and always false) must fail the oracle.
The original void fixture sources and callers stay unchanged.
