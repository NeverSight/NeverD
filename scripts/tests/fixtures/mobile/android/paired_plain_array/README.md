# Two independent seeds with a closed array context and result

Owned Java 8 inputs call a private static method with two independently scoped
Object-bounded formals, one plain Object array and an int index. Its result is
also a plain Object array; no generic result identity is inferred. Frozen
observers check independent/null seeds, input/result identity, ordered writes,
partial effects, class initialization and exact failures. The SDK profile keeps
Trace's exact original declaration bytes while recovering the private helper.
