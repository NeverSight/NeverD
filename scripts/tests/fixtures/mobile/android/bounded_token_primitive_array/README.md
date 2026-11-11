# Owned bounded token with primitive array context

A Number-bounded method retains byte[], Class<N>, ByteOrder, its own N result
and an original IllegalArgumentException declaration. The provider returns an
opaque Number and records array/token/order identities; callbacks mutate the
original bytes and result. Four immutable routes run 96 fresh JVM cases twice.
Source and SDK-provider profiles compile all output and preserve metadata and
Exceptions. Null/empty arrays, signed bytes, mismatched/non-Number tokens, null
orders, opaque results and exact callback/late failures remain observable.
Six wrong programs and six Code-preserving controls guard the binding.
Only owned fixture and standard SDK code executes. Use JDK 25; originals use
release 8 and recovery uses its reported Java release.
