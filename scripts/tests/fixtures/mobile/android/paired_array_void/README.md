This owned fixture preserves `<K,V>(Sink<? super K,? super V>,
Pair<? extends K,? extends V>[])void`. Source and frozen provider/pair/consumer
profiles each run115 fresh JVM cases, two attempts per case, through source,
frozen client, frozen direct provider, reflection and method-handle routes.

Frozen getters replace external input/consumer references, replace the current
array element and change its value. Frozen put implementations replace the next
array element and expose exact key/value identity, calls and partial writes.
Raw and compiler-bridged typed consumers witness late argument checks; arbitrary
raw put return values must remain discarded. Independent observers check array
and element snapshots, getter order, forward traversal, null/empty inputs,
nullable fields, RuntimeException/Error identity and first/repeated static
initialization failures. Full reflection checks separate own Object formals and
both complete upper producer/lower consumer slots in their original order.

Every generated Java file compiles. Independent SDK and compiler inventories,
complete access/Signatures/fields/class relationships and Exceptions must match.
Ten unchanged-Code Signature mutations reject; ten independently compiled wrong
programs must fail the immutable observer. Original classes, DEX and observers
are hashed before recovery and checked again afterward. Native cases also cover
all16 invariant/variant combinations, source/SDK declarations, java.util.Map and
custom consumers, inherited targets, input ordering and ordinary/range calls.
