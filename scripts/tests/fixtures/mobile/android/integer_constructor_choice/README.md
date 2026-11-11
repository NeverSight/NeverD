# Integer constructor choice

A Java reference and an independently authored ASM Core expose the same API.
The ASM original allocates once before comparing a captured int with zero, then
selects its no-argument or int constructor. Both originals must execute and
agree before D8 and recovery. D8 must preserve that allocation and both calls.
The builder emits Java 8 bytecode; it does not rewrite a compiler original.

Both comparison polarities run with source and SDK constructor declarations.
Four frozen call routes cover 128 fresh JVM cases twice per layout, including
negative, zero and positive inputs, integer extremes, class initialization and
sticky failures, changed global inputs, partial final stores, constructor and
handler exceptions, and Error filtering. Ten independently wrong programs must
fail the observer. Every emitted Java unit compiles; all original class and
method metadata, Code roles and Exceptions match, with exact helper accounting.
