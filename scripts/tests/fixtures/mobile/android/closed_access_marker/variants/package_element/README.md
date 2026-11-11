# Closed carriers of an owned package static member

A package-level final owner has a private constructor whose invariant Set
contains its own package-level static Value member. Its compiler access
constructor forwards the unchanged raw Set through an unused anonymous marker.
The element declaration stays owned in both source and SDK callback layouts.

Four frozen caller routes cover 68 fresh JVM cases twice. They check exact
member visibility and generic metadata, raw set identity, poisoned elements
that fail only on later reads, callback mutation, partial final stores and
checked/runtime/Error identity. Every generated Java unit is compiled and all
original definitions and metadata are compared, with the existing anonymous
marker and synthetic access projections individually accounted for. Eleven
wrong programs and four Code-preserving Signature controls must be detected.
