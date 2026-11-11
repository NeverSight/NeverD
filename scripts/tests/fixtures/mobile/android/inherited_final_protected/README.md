# Inherited final protected generic provider

An abstract Provider<E, C extends Box<E>> owns a protected final callback and its
original public erased bridge. Core<E> specializes C to NarrowBox<E> and inherits
the implementation. The input encoding changes only the compiler bridge from
protected to public, preserving its Code attribute.

Recovery widens only the typed provider's source visibility and keeps its final
modifier. Complete class-owned C and E scopes, all runtime entries and every
other class/method/Exceptions attribute are compared after compilation. Core
acquires no virtual methods. Base and Box also run as immutable declarations.

The immutable caller and harness compare typed, inherited, reflection and
method-handle routes, aliases, nulls, invalid erased inputs and exception order.
The provider's C bound remains the original runtime cast even though the child
specializes it to a narrower class. Same-Code controls change class/value scopes
or remove the synthetic flag; independent javac rejects an incompatible parent.
Native controls cover final generic results and void callbacks, source order,
illegal descendant overrides, extra access flags and corrupted register origins.

The runtime matrix checks 294 values, 868 identities, 388 states and 300 ordered
failures. The source profile has five classes and ten methods; the dependency
profile has three classes and six methods.
