This owned fixture retains `<C extends Collection<? super Character>>
(CharSequence,C)C` in source and frozen-provider modes. Four routes, two caller
operations and25 modes run200 fresh JVM cases per stage, two attempts each.
Reflection checks the one closed unary carrier bound, lower Character slot,
exact own C seed/result and original fixed CharSequence context.

Mutable text and collection callbacks observe repeated length queries,
length growth/shrinkage, changed next UTF16 units, surrogate pairs, unsigned
characters, NUL/cache boxing identity and exact charAt/add order. Callbacks
replace external text/destination references; original arguments remain used.
False add results stay discarded, partial additions survive original failures,
and returned alternate/null collections retain identity and late caller casts.
RuntimeException/Error, later-query/read/add failures, raw nulls and first/
repeated static initialization failures stay exact. Text conversion is forbidden
by the frozen callback, independently of baseline-output comparison.

Every generated Java file compiles; SDK/compiler inventories, full metadata,
Signatures and Exceptions must match. Eight unchanged-Code Signature mutations
reject and ten independent wrong implementations must fail the frozen observer.
Original classes, DEX and observer bytes are hashed before recovery and checked
afterward. No private APK or generated private Java is executed.
