# Closed visibility result bridges

An owned public final child inherits a class-formal result from a package
abstract parent. The original compiler creates a public synthetic bridge with
the unchanged interface return descriptor. Source and SDK layouts must regenerate
that exact entry and forwarding bytecodes, with all original identities, Code
roles, access, Signature, Exceptions and parameter metadata retained.

Frozen callers, reflection, method handles and rebuilt callers distinguish erased
results from caller-side narrowing. Heap pollution, nulls, future returned
subclasses, callback mutation, reentry and exact checked/runtime/Error identity
expose an early or invented cast. Independent wrong programs and unchanged-Code
Signature controls qualify the observer before recovery.
