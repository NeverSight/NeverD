# Scoped defaults with fixed exception declarations

The owned `Api<T>` has a default returning its class formal, a complete
`Carrier<Number, String>` parameter and ordered fixed checked `First, Second`
exceptions. `Core implements Api<Core>` overrides it and keeps its original
Object-return bridge with the inherited ordered Exceptions attribute.

The immutable `FixedExceptionDefaultsHarness.java` checks direct, interface,
reflected typed/bridge and independent subclass dispatch, plus the untouched
default on an independent implementation. It checks 33 values, 966 identities,
100 state observations and 73 failures, including mutable carrier aliases,
original checked exception identities and casts after target effects.

Source and immutable interface-JAR profiles retain five classes/seven methods
and four classes/six methods, full metadata and all original runtime entries
apart from reported private helpers. Coverage remains partial solely for the
original compiler bridge. Same-Code full parameter and non-Throwable ancestor
mutations fail NeverD and independent javac. Native controls cover unchanged
runtime slots, empty narrowing, source/SDK roots, original bridge flags/body
and missing closed exception receipts. Original inputs, harness and SDK hashes
remain unchanged.
