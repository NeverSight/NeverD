# Nested lock scopes

Owned Java 8 two-lock synchronization with callbacks before, inside and after each lock scope, inner and outer finally blocks, and mutable state. Separately compiled immutable callers directly inspect Thread.holdsLock at every callback. Distinct and reentrant lock identities, null acquisitions, failures inside either scope, and finally exceptions replacing older failures must retain original ordering and release boundaries.

Source and SDK-callback variants compare original definitions, metadata and Exceptions and compile every generated Java file. Four call routes exercise 20 modes twice. Independent wrong implementations must be caught by the unchanged observer.
