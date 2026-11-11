# Synthetic interface roots

Genuine Kotlin/JVM8 `@JvmSynthetic` output retains an abstract contract, a
concrete default and two independent implementations of a non-Java identifier.
A subinterface and genuine Kotlin callers exercise inherited virtual/default
lookup. Source and SDK-callback profiles compile every generated Java unit and
compare96 fresh JVM cases twice through reflection, MethodHandle and compiled
callers, checking wide/scalar words, NaN/zero bits, null receivers, token identity
and partial callback failures.

Every original definition and metadata/Exceptions is compared. The four source
name bindings and synthetic flag losses must be disclosed individually; genuine
D8's CLASS annotation loss is independently verified. An unchanged SDK root must
remain rejected. Fourteen independently compiled wrong callbacks must fail.
