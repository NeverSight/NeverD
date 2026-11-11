# Inherited protected generic provider

An abstract Provider<E, C extends Box<E>> implements a protected parent slot
through a typed read(C) method and its original erased bridge. A final Core<E>
specializes C to NarrowBox<E> and declares only a constructor. The input encoding
changes only the original provider bridge from protected to public; its Code
attribute is unchanged.

Recovery must consume the complete inherited provider contract without adding
virtual methods to Core. It reports the typed provider access change to public and keeps
all other original signatures, fields, methods and exception declarations. Base
and Box also run as immutable dependency declarations.

The independent harness and package caller are compiled before recovery. An
immutable external subclass overrides the narrower protected method and retains
its own checked bridge. Six caller routes compare original virtual dispatch,
nulls, aliases, invalid erased inputs, state changes and exception ordering. A
plain Box remains accepted by the provider bound but fails at the external
subclass's narrower bridge. Source controls reject incompatible parent arguments,
method-owned result scopes and missing synthetic flags. Native controls also
reject input descendant redeclarations and altered incoming registers.

The JVM matrix checks 56 values, 381 identities, 147 states and 81 failures. The
source profile has five classes and nine methods; the dependency profile has
three classes and five methods. Core retains its constructor and the reported private static throw helper; it
acquires no virtual methods.
