# Closed generic interface defaults

This original Kotlin compiler fixture reuses the independently authored Java
contracts in generic_default_access. Its default method has fixed parameterized
input and result types with no method formals. Frozen callers and reflection
verify complete signatures, direct default versus virtual override dispatch,
receiver/argument/result identity, wide data, partial construction and failures.
No application data is used.
