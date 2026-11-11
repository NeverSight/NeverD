# A freshly allocated callback before a generic superclass constructor

The ordinary Java reference allocates a raw Supplier before calling the sole
fixed-arity Base<T>(Supplier<? extends T>) constructor. Adapter initialization
captures a reference before the parent mutates it. All source inputs and frozen
observers are compiled before D8; no original classfile or DEX is rewritten.

Four call routes, nine fresh JVM modes and two attempts observe initialization
failure caching, exact constructor/callback order, the captured reference,
callback identity, partial parent and child fields, null markers and original
exception identity. Independent wrong programs must fail the same oracle.
Recovered Java uses release 25 for the original pre-super allocation statement;
all original definitions, metadata and Exceptions are compared in source and
dependency-parent modes.
