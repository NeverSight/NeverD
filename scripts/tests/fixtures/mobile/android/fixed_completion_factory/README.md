# Fixed completion results with independent callback formals

Original Java8 declarations have unary and seeded static factories returning the
same unary completion carrier parameterized by one closed nominal leaf. Unary
callbacks and completions share one Object-bound method formal. Seeded callbacks
also consume an independent Object-bound receiver formal. Class and method formal
ownership, full carrier declarations, variance and return metadata remain exact.
A separate unmodified Kotlin compiler study exercises the real coroutine API shape.

Source and SDK-factory/carrier profiles compile every emitted Java8 unit, compare
every original definition, supported metadata and Exceptions, and run 168 fresh
JVM cases twice through reflection, two MethodHandle routes and direct Java.
Independent assertions observe null/aliased/arbitrary returned carriers, receiver,
callback and completion identity, opaque callback results, optional execution,
partial effects, return snapshots and exact runtime/error timing. Eighteen wrong programs
must fail before recovery. Generic binding never predicts provider execution.
