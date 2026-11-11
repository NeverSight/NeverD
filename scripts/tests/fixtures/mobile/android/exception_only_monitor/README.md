# Exception-only monitor release

Owned Java 8 methods always throw after a callback while holding the original
receiver or argument lock. D8 must retain one acquisition and one exceptional
release per method. The declared synchronized instance method must keep its
method flag without a second explicit acquisition.

Frozen direct, compiled, reflection and MethodHandle callers compare callback
identity and order, partial effects, exact checked/runtime/Error failures,
null receivers and probes, reentrancy and repeated exception allocation.
Bounded contention checks identify the exact blocked lock and owner. Source
and SDK-provider profiles compile all emitted Java and compare every original
class, method, Code role, access flag, Signature and Exceptions value. Twelve
independently compiled wrong programs must fail the same observer.
