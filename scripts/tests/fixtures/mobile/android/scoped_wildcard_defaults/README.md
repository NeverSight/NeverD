# Class-scoped wildcard source defaults

The owned source interface retains `Root<K, Entry<K,V>>` and its original
`putAll(Map<? extends K, ? extends Entry<K,V>>)` default. No bridge is needed:
the runtime descriptor agrees with the root. The generic input consumer declares
no `putAll` method. The root can remain source input or be an immutable JAR.

The immutable `ScopedWildcardDefaultsHarness.java` checks typed/raw/reflected
calls, an independent override, null maps and receivers, mutable input aliases,
original exception identities and a value cast after key effects. Each profile
checks 28 values, 256 identities, 52 state observations and 24 ordered failures.
Complete compiler metadata and inventory remain unchanged apart from reported
private helpers. The source profile has five classes/five methods and the
dependency profile four classes/four methods, with zero bridges or projections.

Changing only nested invariant Signature arguments while retaining Code and
runtime descriptors fails NeverD and independently authored javac source.
Original input, immutable harness, dependency JAR and SDK hashes stay unchanged.
