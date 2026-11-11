Genuine Kotlin/JVM source defines a generic Key<T>/value pair on an abstract void
method, then an object override returning Nothing. The compiler emits a public
<T> Void target and a raw void bridge with identical input descriptors. The target
allocates IllegalStateException, stringifies its literal message, initializes the
exception and throws; the bridge invokes it and discards the result.

A frozen external observer selects exact return descriptors through reflection,
two MethodHandle routes and an inherited generic API control. It checks the own
method formal, raw bridge Signature, null/arbitrary/aliased values, untouched
operand probes, fresh exception identity, message, cause and exact Core frame
structure. Source naming differences and irreducible bridge flags are explicit;
the original raw bridge must not gain invented generic formals. Only owned code
is executed. Source and SDK-key/base profiles preserve the same original DEX.
The original Kotlin debug SourceFile is Core.kt; javac records the exact emitted
top-level Java filename. The runner checks and records this specific difference
separately from the unchanged signatures, annotations, flags and Exceptions.
