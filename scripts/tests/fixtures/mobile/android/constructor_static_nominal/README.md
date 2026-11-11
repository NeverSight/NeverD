# Owned static nominal results before generic super

Two descriptor-only static factories return opaque Strings before a raw callback
is allocated and passed to Base<String>'s (T,T,Callback<T>,boolean,boolean)
constructor. Preserve input/result identities, aliases, nulls, captured context,
signed indices, scalar flags, partial constructor state and exact failures.
Four immutable constructor routes run 96 fresh JVM cases twice. Source and
SDK-factory profiles compile every unit and preserve metadata and Exceptions.
Six wrong programs and six Code-preserving controls guard the binding.
Only owned fixture and standard SDK code executes. Use JDK 25; originals use
release 8 and recovery uses its reported Java release.
