# Bounded formal superclass calls

Independent Java 8 keeps a protected static nested child with its own bounded
formal across two generic superclass edges. Its public Boolean method invokes
the inherited original implementation and then writes child state. Complete
class and method scopes remain distinct; a narrower class bound never guesses
an argument value or supplies a new runtime check.

Frozen callers include an independently compiled future subclass, erased bridge
entries, reflection and MethodHandles. They observe receiver/argument identity,
opaque Boolean results, partial writes, original bridge casts, callbacks and
exact checked/runtime/Error objects. Source and SDK variants compile all output
and compare every original entry, role, Code presence, Signature, Exceptions and
MethodParameters. The fixture contains no private application code or identity.
