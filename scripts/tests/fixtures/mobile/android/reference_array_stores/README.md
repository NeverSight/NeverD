# Reference array stores

These authored Java programs use erased reference views of String arrays and
primitive subarrays, preserving the runtime component check of each array store.
Frozen direct/reflection callers check ordinary stores, fresh arrays, constructor
argument stores and loops. Observers compare original array/value identities,
partial writes, callback order and failure identity, null/index/type exception
precedence, and first/repeated outer class initialization failures.
