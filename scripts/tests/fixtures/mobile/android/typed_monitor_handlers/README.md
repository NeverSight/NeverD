# Ordered typed monitor handlers

Owned Java 8 declarations retain ordered typed catch clauses and finally
effects while the original monitor remains held. A second entry nests this
lifetime under another original acquisition.

Frozen direct, nested and method-handle callers check typed selection,
unmatched Error identity, exceptions originating in handlers, overriding
finally failures, return snapshots and release on every exit. Callbacks check
both original lock identities. Source and SDK-provider modes compare original
definitions, Signatures and Exceptions metadata. Counterexamples must fail
the same observer.
