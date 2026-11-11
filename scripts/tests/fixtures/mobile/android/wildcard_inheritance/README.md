# Wildcard bounds across declaration-only inheritance

Owned dependency headers define `Leaf<E> extends Middle<E> extends Root<E>`.
The single input class uses Leaf wildcards as bounded arguments of covariant,
contravariant and unbounded containers. The normal javac output is supplied to
D8 without modifying any input metadata or bytecode. The dependency JAR stays
outside the input inventory and recovered sources.

The JVM oracle compares 54 choices, 36 ordinary reads, mutable reference
identity and exact exception identity. Reflection compares each field's full
generic type with its method's result and parameter types. Every original
method descriptor and all generic metadata stay unchanged.

A second JAR changes only the covariant container's class Signature to require
an invariant Root argument. NeverD rejects the existing DEX without publishing
output, and independent javac rejects the original input source against that
same JAR. Component cases also reject reversed directions, lost invariance,
nested/array capture rewrites, unknown dependency variables, cycles and
conflicting outer-instance formals, while retaining identity self bindings.
