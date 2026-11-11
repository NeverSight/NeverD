# Scoped source default parameter bridges

The owned source interface retains `Root<K, Entry<K,V>>` and its original
`put(K, Entry<K,V>)` default. Javac supplies a checked `Object,Object -> Object`
bridge in that interface. The generic input consumer declares no `put` method.
The root can remain input source or become an immutable dependency JAR.

The immutable `ScopedDefaultParametersHarness.java` checks the original input
and recovered classes with an independent subclass, typed/raw/reflected dispatch,
nulls, mutable aliases, effects and original exception identities. Each profile
checks 43 values, 364 identities, 76 state observations and 33 ordered failures.
Complete class/member metadata and runtime inventory remain unchanged apart
from reported private helpers and one regenerated bridge. The source profile
has five classes/six methods; the dependency profile has four classes/five
methods. Coverage remains `partial`, with no generic metadata projection.

The runner changes only the typed method's invariant result arguments or the
original bridge flags while retaining Code and descriptors. NeverD rejects both;
independently authored Java also rejects the changed generic result. Original
input, immutable harness, dependency JAR and SDK hashes remain unchanged.
