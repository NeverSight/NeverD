# Shared nominal upper bound fixture

All classes and method bodies are independently authored. The runner compiles a
generic static `Worker<T extends UpperBound>` member, then changes only its class
Signature and enclosure metadata to model an anonymous method-local class using
the original `Owner<T extends UpperBound>` scope. Every original Code attribute
and runtime descriptor is compared before and after the edit.

The original factories and the worker's copy method allocate the same class, so the
recovered source must share that one identity. The nominal upper bound has no type
parameters or additional bounds. Class, field, constructor and result signatures
use the captured variable; a self return also exercises captured anonymous arguments
whose original reflection raises `MalformedParameterizedTypeException` after the
class formal is removed. That separate metadata difference is explicitly checked.

The D8/JVM oracle checks 32 returned identities, two concrete subclasses, mutable
values, shared capture identity, copied null storage, 26 ordered failures, every
runtime method, and the five separately reported generic reflection differences.
The original owner's bound remains intact. No private artifact is included.
