# Allocated nested carrier delegation

Independently authored constructors keep two owner formals, a generic parent,
a primitive caller without a Signature, and `Map<Wrap<K>, Pair<K,V>>` in the
selected target. One caller allocates an owned generic `HashMap` subclass;
another allocates the standard class itself. The owned initializer has observable
class initialization, callbacks, arbitrary raw contents and exception identities.

Frozen callers and observers check allocation and partial-owner identity,
constructor order, two wide argument patterns, changed external state and the
later `Pair` cast without predicting collection contents from generic metadata.
Source and dependency profiles retain every original definition and metadata
attribute, compile all emitted Java, and reject independent wrong programs plus
Code-preserving unsupported Signature mutations.
