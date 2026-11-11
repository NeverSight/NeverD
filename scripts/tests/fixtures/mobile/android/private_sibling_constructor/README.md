# Closed constructor types in an owned private lexical scope

Three private static members share one public enclosing declaration. A private
constructor retains a sibling context with a closed Supplier header and an
invariant List of another private sibling. The compiler's synthetic access
constructor forwards those inputs and a shared constructorless marker.

Four frozen callers compare 64 fresh JVM cases twice, including reflection on
the access constructor. They check private/static enclosure, exact generic and
exception metadata, raw list and context identity, partial final fields,
callback ordering, checked/runtime/Error identity and delayed element casts.
All generated Java is compiled; the existing marker-name and synthetic access
flag projections must be individually disclosed and accounted for. Public
callback declarations are tested as both source and SDK dependencies.
