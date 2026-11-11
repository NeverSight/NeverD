# Nominal method bounds and full substitution

This independent interface JAR declares make(Pair<C, M>) -> Pair<M, M>
with M extends Bound. The owned implementation uses an alpha-equivalent R,
retains a distinct class formal T and passes through raw stored references.
The original methods need no bridge and retain their runtime descriptors.

The JVM oracle checks raw and typed inputs, reference and null identity,
mutable storage, interface and subclass dispatch, target failure identity,
and full method bound and scope reflection. The counterexample changes only
the implementation's method bound from Bound to OtherBound; both methods still
have the exact Pair-to-Pair runtime ABI and unchanged Code. NeverD and javac
must reject this changed override.
