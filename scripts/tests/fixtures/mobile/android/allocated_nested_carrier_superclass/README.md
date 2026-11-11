# Allocated nested carrier superclass calls

Independently authored `Core<K,V>` constructors pass an actual fresh collection
to `Parent<K,V>(long, Map<Wrap<K>, Pair<K,V>>)`. This fixture reuses the owned
`Base`, `Store`, `Trace`, `Wrap`, `Pair` and frozen caller/observer from
`allocated_nested_carrier_delegation`. Parent fields and getters are inherited
without inventing child definitions.

The runner's `--superclass` source and SDK-provider profiles compare every
original definition and metadata attribute, compile all generated Java, and
retain allocation, callback, class-initialization and exception behavior. D8 may
reuse an unused incoming register in the standard collection path; both paths
must retain the original wide argument and complete scoped target contract.
