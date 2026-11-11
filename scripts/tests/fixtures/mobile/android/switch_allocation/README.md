# Switch constructor arguments

Owned Java 14 input allocates `Target` before a packed switch. Three arms call an
opaque provider; the default constructs an exception using another allocation
and throws without initializing `Target`. Captured input values precede the
allocation. The observer changes global values during class initialization,
provider calls and partial construction, and injects exact RuntimeException or
Error objects. Two attempts in each fresh JVM check successful initialization
and failed initialization caching. Null arguments and invalid selectors remain
observable. Source and dependency profiles compile every recovered Java unit,
compare every original definition and metadata, and reject independent wrong
programs. No private sample data is used.
