# Conditional array arguments

Independently authored Java 8 input creates a result before choosing an argument,
then allocates a dynamically sized String array through an Object-array view.
Two ordinary reference stores remain between the original allocation and its
initializer. Callback replacement checks the captured receiver of the final
argument call; a callback may return an opaque different array.

Frozen direct, compiled, reflection and MethodHandle callers run both branches
in sixteen modes, twice per fresh JVM. They check class initialization, partial
array writes and final fields, negative/empty/short arrays, null elements,
covariant store failures and exact checked/runtime/Error identities. Independent
DEX inspection qualifies the new-array and aput-object operations. Wrong
programs must fail the observer. Source and SDK-provider profiles compile every
emitted unit and account for original definitions, flags, Code, Signature and
Exceptions; the original provider classfile is never replaced with a stub.
