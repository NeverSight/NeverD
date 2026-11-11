# Plain class chains and inherited generic members

The named static ancestor fixture gains four class edges and a nonempty plain
interface on its distant root. The original root owns method formals, complete
parameterized method types, construction effects and mutable state. Source
and unchanged ancestor-JAR profiles preserve all metadata, runtime methods and
original generic interface default ownership, without projections or bridges.

The immutable wrapper retains all recursive default and static scope checks,
then verifies distant construction, method scopes, reference identities,
interface aliases, virtual overrides and effects before direct/reflected
failures. A same-Code classfile changes only a generic ancestor parameter's
argument count. The source and altered dependency profiles must reject it,
while an independent javac check rejects the same invalid arity. Native tests
also cover Signature-free receivers, class order, several depths, unknown
roles, captured scopes, cyclic edges, final overrides and conflicting abstract
interface requirements.

`--ancestor-throws` adds the same checked exception to the root interface,
ancestor and input override. The immutable wrapper compares the original
Exceptions metadata and ordered direct, interface and reflected failures.
Native cases also retain differently ordered equivalent sets and empty
narrowing, while unrelated, added or unknown checked exceptions still fail.
The complete-method relation uses the common compatibility receipt; original
bridge exception equality remains strict.
