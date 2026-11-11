# Closed map constructor arguments

A descriptor-only child constructor passes an exact nongeneric Snapshot
subclass to a generic HashMap constructor parameter, then saves the original
Snapshot reference. The frozen observer checks nulls, mutable contents,
initialization failures, exact throws and partial parent/child fields through
four independent constructor call routes. All original methods, Signatures
and Exceptions remain accounted for in source and SDK parent profiles.
