# Comparable scalar predicate bridges

A final class implements two independently declared interfaces with the recursive
bound `T extends Comparable<? super T>`. Original class metadata binds both to
`Character`, while the original public synthetic bridge keeps its `Comparable`
parameter, casts to `Character`, calls `charValue`, and dispatches to the same
owner's original `char` predicate. Covariant getters retain both original return
slots. An unrelated parameterized superclass retains its complete arguments.

The fixture compiles the erased source form and immutable callers first. Its
encoder changes only the two original interface arguments and predicate bridge
flags, with unchanged Code attributes. The same harness then compares direct,
raw/typed interface, reflection, MethodHandle and original caller routes. It
checks char boundaries, parent mutations, shared arrays, getter identity, null
and wrong-class failures before dispatch, and exact original exception objects.

The recovered source uses raw versions of the two interfaces and an ordinary
Java declaration for the original predicate body. These metadata differences
are reported explicitly; original method descriptors, calls, fields, superclass
arguments and Exceptions attributes remain unchanged. Recompilation must retain
the exact original runtime inventory plus reported private helpers. Source and
immutable ancestor-JAR profiles share this oracle. Same-Code wrong arguments
and missing bridge flags must fail. Independent javac also rejects the
unprojected parameterized interface with the erased source declaration.
