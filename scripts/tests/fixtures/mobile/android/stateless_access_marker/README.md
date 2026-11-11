# Stateless anonymous implementation as an access marker

The Java 8 compiler reuses an anonymous implementation as the unused last
parameter of a synthetic constructor. The same class has an original constructor
and callback body, implements a private member interface, and is allocated by
its enclosing class. Its constructor does not capture state.

Both source and SDK layouts must compile every emitted unit, account for every
original definition and Code/access/Signature/Exceptions fact, and disclose the
existing anonymous-to-named mapping. The original anonymous constructor must
remain an original method; no replacement default constructor is allowed.

Four frozen call routes run 64 fresh JVM cases twice. They distinguish null and
real unused marker arguments, a private context, opaque collection identities,
partial final fields, delayed poisoned-element casts and exact failures. The
same marker instances also execute the original callback before and after
construction, checking callback count, input/result identity, mutations and
checked/runtime/Error propagation. Sixteen independently compiled wrong programs
and four Code-preserving Signature controls qualify the observer and binder.
Only authored fixtures execute.
