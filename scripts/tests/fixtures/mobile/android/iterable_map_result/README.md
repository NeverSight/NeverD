# Iterable pair input and nested binary result

Independently authored Java8 declarations retain Iterable<? extends Map<K,V>>
and Map<K,List<V>>. Frozen observers check exact iterable/key/value references,
iterator/next/map-get order, opaque result and nested aliases, partial runtime
and Error identity, null results, ignored calls and delayed HashMap casts.
Source and SDK modes compile every output and compare original metadata,
roles, Exceptions and helpers. No private application bytes or behavior are
included.
