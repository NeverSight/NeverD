# Known bound, contexts, and invariant tokens

This owned provider declares `<T extends Serializable> T read(Context, String,
Class<T>)`. The invariant token binds exactly the same known bound as the
result. Complete bound declarations, static owner scope, context declarations,
original descriptors and package access remain mandatory. Native cases also
exercise a class bound, static nested entries and source/SDK declarations.

Public-source, package-source and SDK profiles preserve complete original
metadata and frozen observers. Fresh JVMs check original context and token
identity, virtual callbacks, live provider fields, result snapshots, late casts,
ignored results, mutable values, null/cast order and exact checked/Error
identity. A token never predicts a narrower result value. Deliberately wrong
programs must fail the independent observer.
