# Paired generic array boolean operations

This independently authored provider has ordinary and variable arity methods
with two arrays of the same method formal and an opaque boolean result. Its
own formal shadows a differently bounded class formal. Every variable arity
call supplies two original arrays directly.

The observer checks array identities, null/empty/aliased/covariant arrays,
element snapshots, array-store failures, optional callbacks, callback mutations,
ignored calls and exact exceptions. The provider chooses its result independently
of array equality. Source and dependency profiles compile all emitted Java8
units and preserve definitions, metadata, roles and Exceptions. Wrong programs
run before recovery, including explicit varargs packing and comparison shortcuts.
Signature-only controls preserve Code and runtime descriptors.
