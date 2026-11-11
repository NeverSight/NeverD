# Iterator, map seed and scoped key/value callbacks

Independently authored Java8 declarations retain a complete binary map seed
beside an invariant iterator and either a key callback or separate key/value
callbacks. The callback interface preserves Serializable and checked Exceptions.
Frozen observers check distinct references, callback/mutation order, partial
runtime/Error/checked identity, nulls, opaque results and ignored calls. Source
and SDK modes compile every output and compare metadata, roles, Exceptions and
helpers. No private application bytes or behavior are included.
