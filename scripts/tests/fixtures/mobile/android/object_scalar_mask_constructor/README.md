# A non-public final generic owner's default constructor

Genuine Kotlin emits one Object-bounded class formal, an Object/int/int/String
primary constructor and an unchanged descriptor-only synthetic default mask
constructor. The final owner is package-private at the JVM level. Its defaults
change only the second integer and the string; all masks and marker arguments
remain original bytecode. Parameter/call assertions are explicitly disabled in
the owned compiler profile, matching the observer's nullable JVM string inputs.

The frozen observer checks every low mask combination through reflection and
MethodHandles, including ignored bits, nulls, opaque object identity, primitive
boundaries, string identity, partially initialized final fields and failures.
Every emitted Java unit is compiled. Complete metadata differences are limited
to reported source helpers/synthetic constructor flags, Java25 class versions,
the Kotlin/Java source filename and explicitly inventoried D8 CLASS annotations.
