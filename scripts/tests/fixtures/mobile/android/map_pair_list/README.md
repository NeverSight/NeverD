# Binary carrier to a sequence of pairs

A public static method in a package-private implementation class receives
`Map<? extends K, ? extends V>` and returns `List<Pair<K, V>>`. A public facade
inherits that entry. The input, outer result and nested pair declarations each
retain their independent known Object bounds and method-formal scopes.

The provider deliberately returns a separately cached list. Neither map
contents nor the Signature predicts its actual entries, pair fields or identity.
The frozen caller checks null and empty results, wrong element/field types,
virtual map reads, mutable callbacks, field snapshots, exact exception and
Error objects, discarded results and original later checked reads.

Source and SDK profiles compile every recovered Java 8 unit and compare all
original definitions, metadata, Exceptions and helper roles. Each runs 64 fresh
JVM cases twice. Signature-only controls preserve Code; independently compiled
wrong programs must fail against the same frozen caller. Only authored code runs.
