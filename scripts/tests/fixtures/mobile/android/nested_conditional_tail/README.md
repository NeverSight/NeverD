These untouched Java 8 compiler originals allocate an outer object, allocate an
inner object with conditional/fallback arguments, initialize that inner object,
then call an opaque wide-result provider and read another volatile outer
argument. Class initializers mutate the source field before the conditional
read. Constructors publish partial objects around every final store.

Frozen direct, precompiled, reflective and method-handle callers make two
attempts in fresh JVMs. Observers check both initialization sites, single volatile
reads, dynamic fallback results, wide return snapshots, later argument reads,
partial object defaults/stores and exact failures at every stage. Exception and
Error initialization failures must remain sticky. Source/SDK providers and both
null-branch polarities are independent profiles; every Java file, original
definition, complete metadata and Exceptions is checked after recovery.
