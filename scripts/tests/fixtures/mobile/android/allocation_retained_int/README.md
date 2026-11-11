# Retained int bridge calls inside nested allocations

An independently authored final primitive wrapper delegates virtually to an
open, effectful getter. An access-only ACC_BRIDGE encoding leaves every Code,
descriptor, Signature and attribute byte unchanged; both original and encoded
programs execute before recovery. Base/overriding getters each run with owned
or SDK API declarations. Frozen callers observe class allocation/init order,
repeated opaque results and mutation, wrapper stack frames, partial stores and
exact first/second-call or constructor failures. Source keeps the original
wrapper body, with its unspellable bridge flag disclosed. All Java, entries,
metadata and Exceptions are checked; wrong programs bypass wrappers, cache or
precompute calls, change receivers/results or move final stores.

The plain API variant removes the unused API formal and its implementation
arguments before independent compilation. Neither class then has a Signature.
The same frozen observers cover source/SDK APIs and base/derived getters;
reflection verifies that recovery preserves this plain interface edge.
