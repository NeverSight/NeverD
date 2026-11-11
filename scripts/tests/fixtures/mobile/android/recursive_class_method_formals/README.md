# Recursive class method bounds

This owned fixture implements an unchanged interface JAR with alpha-renamed
`R extends Bound<R>` method formals. Class formals in `Pair<T, R>` stay distinct
from the method formal and the complete recursive class-bound argument.
The immutable JVM harness checks 53 identities and four ordered failures,
mutable state, virtual overrides, raw calls and generic reflection. A same-Code
bound change to `Bound<? extends R>` is rejected by NeverD and independent javac.
