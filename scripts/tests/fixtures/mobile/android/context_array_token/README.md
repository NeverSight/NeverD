# Context arguments and invariant array tokens

The owned provider declares `<T> T[] read(Context, String, Class<T>)`.
Only the invariant token binds the result formal. Ordinary context arguments
remain independent known nominal types. Source, package-source, and SDK
profiles keep original signatures, flags, metadata, exception tables, and
frozen caller bytecode. Native coverage includes package static nested entries
and rejects foreign access and open or missing context declarations.

Fresh JVMs compare direct calls, method handles, and ignored results. An
independent observer checks receiver and argument identity, ordered callbacks,
actual returned array identity, live provider fields, mutations, late array
casts, null/cast precedence, and exact checked exception and Error identity.
The type token supplies no assumption about the returned array's contents or
runtime component. Deliberately wrong programs must fail this observer.
