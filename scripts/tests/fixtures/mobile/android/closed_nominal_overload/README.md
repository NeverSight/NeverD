# Closed nominal views disqualify unrelated generic constructor overloads

Unmodified Kotlin JVM8 output has a descriptor-only primary constructor, a default
mask constructor, and a same-arity secondary constructor taking a closed nested
Function2 signature. A separate plain Handler interface has a complete Object-only
header and no interfaces. Its static view cannot satisfy the unrelated callback
carrier; explicit original descriptor casts retain that selection even for null
and for an external object implementing both interfaces.

Source and SDK-handler/context profiles compile every emitted Java unit at the
reported minimum release, independently compare every definition, supported
metadata and Exceptions, and run 240 fresh JVM cases twice through reflection,
two MethodHandle routes and direct callers. A separately compiled Kotlin invoker
reaches genuine synthetic mask entries without modifying original classfiles.
All mask bits, ignored high bits, null/shared/covariant arrays, handler/callback
identities, factory snapshots, partial fields and exact runtime/error timing are
checked. Eighteen wrong programs must fail before recovery.
