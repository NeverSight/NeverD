# Closed covariant constructor overloads

A public constructor with a complete `Carrier<? extends Bound>` input evaluates
the original runtime class and an opaque key provider before delegating to its
plain String sibling. Neither constructor binds a class or method formal.
The same-owner proof must validate the whole overload family, including the
carrier declaration and its nominal bound, while preserving the original
parameterized declaration and the exact selected descriptor.

Source and SDK profiles compile every generated unit and compare all original
metadata, Exceptions and helper roles. A frozen future subclass and callbacks
check 64 fresh JVM cases twice: null inputs, dynamic classes, mutable keys,
partial initialization, exact exception/Error identity and late field casts.
Eight Signature-only controls retain Code and eight independently compiled
wrong programs must fail. Inputs and baseline run as Java 8; materialized
constructor prefixes require Java 25 for generated source. Only authored code
executes.
