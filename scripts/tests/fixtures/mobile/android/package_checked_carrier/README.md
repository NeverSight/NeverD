# Package generic helper with closed checked failures

Owned Java 8 fixture retains a package static generic helper on an independent
generic owner with a complete interface header. An invariant Task<V> binds the
method's Object-bounded result; its checked Failure is a closed declaration.
The original retry loop and finally callbacks remain observable. No original
classfile or DEX is patched. Frozen raw, typed and method-handle callers check
retry counts, callback order, cleanup failures, result snapshots and exact
exception identity in source and SDK-provider profiles.
