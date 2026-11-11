# Scoped subtype input keeps the original initializer overload

An owned generic child passes a SortedMap<? extends K,? extends V> reference
through the original Map initializer argument beside a more specific SortedMap
overload. The raw allocation keeps the same runtime reference and preserves the
physical Map cast; no element binding is inferred. Four frozen constructor
routes compare 96 cases twice with source and SDK allocations/parents, auditing
every recovered source unit and metadata entry, including a methodless marker.
Observers check exact overload selection, original source identity, copy reads,
callbacks, snapshots, partial writes, exact failures and later key casts. Wrong
programs and Code-preserving Signature controls guard observation and scope.
Only owned maps execute.
