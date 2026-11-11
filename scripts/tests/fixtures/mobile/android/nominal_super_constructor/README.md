# Closed nominal arguments in generic superclass constructors

Unmodified Java 8 classfiles preserve a closed generic superclass signature while
the child constructor itself needs no Signature attribute. Its nominal input
forwards to a unique fixed-arity parent constructor whose class formal erases to
Object. Parent and child descriptors, generic signatures and field types remain
unchanged; the expression-local binding introduces no method Signature.

Source and SDK-parent profiles compile every emitted Java unit and compare all
definitions, declaration metadata and Exceptions. Frozen observers run 144 fresh
JVM cases twice across reflection, two MethodHandle routes and a direct Java
caller. Nulls, distinct subclasses, long boundary values, original reference
identity, parent/child partial initialization and runtime/error timing are checked.
Sixteen independently compiled wrong programs must fail before recovery.
