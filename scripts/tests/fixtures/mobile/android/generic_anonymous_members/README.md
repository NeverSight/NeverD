# Generic anonymous field member fixture

These independently authored Java sources create a real anonymous field
initializer inside `Owner<T>`. The compiler supplies the enclosure and outer
capture metadata; no classfile or method body is patched. The anonymous class
uses the direct parent's Object-bounded `T`, while its own `echo<T>` shadows that
name. Only the captured class uses are projected.

The independent javac/D8/JVM oracle checks 20 returned identities, mutable outer
values, one captured receiver, eight sentinel failures, a captured null receiver,
all runtime methods, retained public owner metadata and the own method formal.
It confirms the two reported reflection differences and the changed member name.
Java 25 recompiles the explicit capture write before the original super call.
No private artifact is included.
