# Protected primitive array getters

Eight final classes specialize a protected Base<T> getter to each primitive
array type. The original input encoding changes only compiler bridge visibility
from protected to public, leaving Code attributes unchanged. Recovery reports the
typed target's public source access while preserving the original return types,
parent arguments, runtime entries, fields and other metadata.

The immutable package caller and harness exercise inherited, typed, reflective
and method-handle routes. They compare null receivers, null/empty/filled arrays,
shared aliases, fresh allocations, negative lengths, original exceptions and
side-effect ordering. The full method and Exceptions inventories are compared
before and after compilation. Base also runs as an immutable dependency.

The source profile has nine classes and twenty-seven methods; the dependency
profile has eight classes and twenty-four methods. Same-Code controls change an
array element, change its rank or remove the synthetic flag. Independent javac
rejects a mismatched array specialization. Native cases additionally cover
multidimensional/reference arrays, nonfinal inherited providers, source order,
invalid returned registers, casts, scopes and access.

The runtime matrix checks 9,376 values, 5,480 identities, 3,456 state observations
and 2,736 ordered failures across all eight primitive element types.
