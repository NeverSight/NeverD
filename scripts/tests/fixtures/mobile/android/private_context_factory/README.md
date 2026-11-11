# Owned private factories with opposite callback formals

Genuine Kotlin emits a private static factory with two Object-bounded method
formals. One callback consumes a fixed nominal context and the first formal,
then produces the second. The reverse callback consumes the second and produces
the first. A public invariant two-slot interface extends the same parent pair.
Parameter and call assertions are explicitly disabled in this owned profile.

A named implementation keeps both original callback references. The observer
checks factory and interface calls, lazy null failures, fixed context identity,
mutable payloads, repeated calls, leaked results and exact exceptions.
Every generated Java unit is compiled and complete compiler metadata compared,
except reported source helpers, Kotlin/Java source filenames and inventoried
D8 CLASS annotations. No bytecode or DEX is patched.
