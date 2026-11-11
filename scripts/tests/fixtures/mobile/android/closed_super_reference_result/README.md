# Closed superclass reference results

The owned `Base<K,V>` receives a fixed key and a complete `ArrayList<String>`
value, then returns the opaque `V` result. `Core` uses an exact superclass call
with direct inputs, explicit original casts, or a discarded result. Both source
and SDK declaration profiles compile every recovered unit and compare original
identities, Code/access roles, Signatures and Exceptions.

Four immutable caller routes exercise 192 fresh JVM cases twice. They observe
future-subclass dispatch, input and returned aliases, nulls, callback order,
partial effects, original input/result casts and exact checked/runtime/Error
objects. A deliberately polluted generic result must fail at its original
result cast when consumed, and must remain uncast when discarded. Independent
wrong programs and same-Code Signature controls qualify the observer and the
complete superclass-binding boundary. Only owned classes execute.

An evolved parent adds a more specific overload while the original child and
frozen callers remain byte-for-byte unchanged. The original runtime matrix
must still match; recovery must refuse a source invocation that could select
that new overload.
