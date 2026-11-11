# Closed reference getter forwarding

Owned Java declarations contain a raw, nonfinal getter forwarder in a concrete
middle class. Its differently named virtual target returns `Box<Throwable>`.
An abstract superclass and a separate interface own that complete result;
a plain final child inherits the original provider without declaring a bridge.
The encoder changes only the middle forwarder's bridge/synthetic flags and
checks that Code bytes, descriptors and all Signatures remain unchanged.

The independently compiled harness includes subclasses overriding either the
named getter or the entry method. Parent, direct, independent caller, reflection,
method-handle and interface routes check nullable mutable results, receiver
identity, ordered effects and exact failures. Recompilation changes only the
reported bridge flags. Both original return Signatures and the original provider,
method names and inventory remain unchanged.

The dependency profile consumes Api, Box and Base from an unchanged JAR. The
runner verifies every class, field, method and Exceptions declaration; same-Code
flag/result-scope counterexamples and an independent javac input reject an
incompatible invariant result. Input, harness and dependency hashes stay fixed.
