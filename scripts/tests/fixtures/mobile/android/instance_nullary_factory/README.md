This owned fixture retains a public instance `<K,V>Pair<K,V> make()` on a
nongeneric owner. Source and frozen provider/result declarations are verified.
Five routes, three operations and15 modes run225 fresh JVM cases per stage,
with two attempts each. A frozen subclass overrides the generic method and
calls its original parent to witness virtual dispatch.

The independent observer checks actual receiver and result identity, separate
method formals and invariant result slots, null receiver behavior without class
initialization, late key/value casts and exact RuntimeException/Error objects.
Provider callbacks replace the externally visible receiver and its result field;
the call must retain its receiver and the captured returned reference. Original
partial state, call counts, failure order and unchanged unrelated receiver are
checked before and after each effect.

All generated Java compiles and independent SDK/compiler inventories, complete
metadata, Signatures and Exceptions must match. Six unchanged-Code Signature
mutants reject; nine separately compiled incorrect implementations must fail
the frozen observer. Original class, DEX and observer bytes are hashed before
recovery and checked again afterward. Native cases additionally cover unary
and binary carriers, complete static nested owners, inherited references,
source/SDK declarations, ordinary/range calls, return-slot permutations and
reversed class order.
