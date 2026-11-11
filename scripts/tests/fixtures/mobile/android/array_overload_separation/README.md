# Descriptor array overload separation

Owned constructors delegate to a String[] overload beside a Collection<File> sibling. Both fresh empty arrays and forwarded null/empty arrays retain the selected fixed-arity descriptor even when the declaration is varargs. The sibling's element is not inferred.

Four frozen caller routes compare 96 cases twice for source and dependency Base declarations. Observations cover exact arrays and writes, superclass/child order, null actions, stage failures, RuntimeException/Error identity and late result assignment. Six wrong programs and six Code-preserving metadata controls test the observer and constructor boundary. No filesystem operation runs.
