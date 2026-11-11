# Inherited method parameter bridge

A parent implements a generic interface using both a fixed key parameter and a
method-owned recursive enum bound. The child overrides only the independent
non-generic method. Its inherited enum method keeps the original parent bridge
and complete method scope, with no new child declaration.

Frozen callers and future subclass overrides check erased bridge dispatch,
opaque tokens and results, callback order, original effects, exact exceptions,
parent enum casts and later caller casts. All owned generated classes compile
with their original definitions, roles, Signature and Exceptions.
