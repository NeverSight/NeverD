# Filled int-array constructor prefixes

Independently authored Java 25 constructors allocate two int arrays, delegate,
and retain a local array across delegation. The shared variant passes the same
array twice. A frozen external observer verifies each partial final-field store,
array identity/mutation, scalar overflow and exact class-initializer, seed,
callback and postlude failures over direct, compiled, reflection and MethodHandle
calls. Both compiler originals and version-only D8 inputs execute before recovery.
All emitted Java, original entries, metadata and Exceptions are checked. Source
and SDK Trace/Hook variants keep providers immutable. Independently compiled
wrong programs alter aliasing, elements, store order or the retained postlude.
