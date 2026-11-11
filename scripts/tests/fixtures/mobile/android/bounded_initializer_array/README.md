# Bounded token with a reifiable initializer array

A public instance factory binds its nominally bounded method result through
an invariant token. Its final varargs parameter is an existing array of a
complete bounded generic interface with an unbounded wildcard. The array
does not bind the method formal. Frozen observers check array identity and
deferred mutation, ordered callbacks, empty/null arrays and entries, exact
failures, partial state, opaque replacement values and late checked reads.
Both source and imported-declaration modes retain original metadata and
compare 64 fresh JVM cases twice, with Signature-only and compiled controls.
