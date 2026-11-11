# Paired adapter inputs

Owned Java declarations connect two directly supplied method formals through one
complete invariant binary adapter and one invariant unary adapter of the result
formal. Both public static methods retain their original generic signatures and
return an independently supplied opaque result. A fixed nominal context and a
separate scalar-only entry exercise the same complete relation, including wide
arguments and ignored results.

The frozen observer checks input and callback identities, callback results,
evaluation order, result snapshots, delayed caller casts, nulls and original
failures in fresh JVMs. Source and dependency profiles compare every class,
method, field, signature and exception declaration after recompilation. Wrong
programs must fail the observer before recovery is attempted. Only owned code
is executed.
