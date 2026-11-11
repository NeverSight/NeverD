# Owned closed-bound superclass fixture

A descriptor-only child constructor forwards its second incoming parameter to
a protected generic parent constructor. The original class header binds the
parent's interface-bounded formal to that same complete nominal interface;
the extra ClassLoader input is stored afterward. Source and SDK-parent profiles
compile every unit and preserve original entries, metadata and Exceptions.
Frozen direct, separately compiled, reflection and MethodHandle callers run
48 fresh JVM cases twice, checking exact service/loader/result identity,
constructor order, partial final fields, fresh children and exact parent,
callback and late cast/null failures. Six wrong programs and six Code-preserving
Signature controls guard the proof. Only owned fixture code executes.
Use JDK 25 for verification; originals compile at release 8 and recovery uses
its reported Java release.
