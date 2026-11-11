# Map seed before iterable key/value inputs

Independently authored declarations retain a complete Map<K,List<V>> seed before
an invariant Iterable and either a key callback or distinct key/value callbacks.
Returned maps stay opaque and may differ from the seed. Observers cover iterable
creation order/failures, callback order, partial mutations, exact identities,
ignored calls and invalid opaque results without inferring grouping behavior.
