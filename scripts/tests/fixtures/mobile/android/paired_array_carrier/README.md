# Covariant binary carrier arrays

Original Java 8 varargs source consumes a one-dimensional array of covariant
binary carriers and returns an invariant carrier of the same two independent
Object-bounded method formals. Frozen callers observe array identity, slot
identity and order, null failures, provider writes, actual result identity and
ordered exceptions. The original array is passed in fixed-arity form, including
through reflection and a fixed-arity method handle. Wrong copies, writes,
slots and results are independently compiled counterexamples.
