# Array length constructor prefix

An owned Java 8 constructor evaluates a callback, reads its byte array length,
and delegates to the constructor that writes its final fields. A later callback
uses the initialized receiver. The original DEX must retain that exact linear
order. Generated source uses Java 25 constructor prologues to keep the original
operations and register state around the delegation.

Frozen direct, compiled, reflection and MethodHandle callers compare null and
empty arrays, input identity, partial final fields, full int values, callback
replacement and exact checked/runtime/Error failures. Source and SDK-provider
profiles compile every Java unit and account for every original definition,
Code role, access flag, Signature and Exceptions value. Fourteen independently
compiled wrong programs must fail the same observer.
