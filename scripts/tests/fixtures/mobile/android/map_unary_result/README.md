# Covariant nested map value and unary pair result

Independently authored Java8 declarations retain Map<K,? extends Iterable<V>>
and List<Map<K,V>>. Frozen observers check exact map/key/value references,
map-get/iterator/next order, opaque result and nested aliases, partial runtime
and Error identity, nulls, ignored calls and delayed ArrayList casts. Source
and SDK modes compile every output and compare metadata, roles, Exceptions and
helpers. No private application bytes or behavior are included.
