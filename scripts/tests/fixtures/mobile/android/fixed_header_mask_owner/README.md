# Bounded owner with fixed nominal headers

Owned Kotlin JVM 8 fixture with a private generic primary constructor, a genuine
package synthetic default constructor and a public producer constructor. Its
single class formal has a known nominal bound, while the superclass and marker
interface are fixed nominal headers. No original classfile or DEX is patched.
The Java parent and value/token callbacks expose escaped partial fields and
constructor order. Frozen reflection, method handles and a direct producer
caller exercise ignored marker/mask bits, nullable values, aliasing, default
selection and exact failures in source and SDK-provider profiles.
