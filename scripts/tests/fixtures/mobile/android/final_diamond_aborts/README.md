# Final assignment diamonds with abrupt exits

Owned Java 8 inputs assign blank final fields through two conditional arms while
retaining earlier and later explicit exception paths. A frozen external caller
observes partial initialization, exactly ordered getter and failure effects,
volatile long snapshots, object identities, nulls, aliases and repeated fresh
allocations through direct, reflection and two MethodHandle routes.

The source and independent SDK-provider profiles each compile every recovered
Java unit and compare all original definitions, class/method metadata and
Exceptions. Each runs 128 fresh JVM cases twice and rejects 14 wrong programs.
The native matrix separately covers inline throws, exact throw-only tail entries,
all ten field types, static initialization and repeated writes on abrupt paths.
No private input or generated private code is used or executed by this fixture.
