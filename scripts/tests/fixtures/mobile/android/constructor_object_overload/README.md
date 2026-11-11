# Object views beside parameterized constructor siblings

This independently authored Subject shares the unchecked factory, trace, Core,
base and immutable behavior harness from guarded_constructor_unchecked_getter.
Its Class<?> constructors delegate to the original Object constructor. The
selected erased Object view cannot select a nominal Class<?> sibling, including
when the actual returned value is null, a Class, an array or an opaque object.
The factory remains generic, varargs and explicitly unchecked-throwing.

Both source and SDK profiles compile every emitted unit at its reported release,
audit original roles, Signatures, Exceptions and helper accounting, and run the
frozen behavior cases twice in fresh JVMs. Wrong programs precede recovery;
additional same-Code controls cover unresolved sibling formals, unsupported
constructor-owned formals and wrong nominal arity. No private input is executable.
