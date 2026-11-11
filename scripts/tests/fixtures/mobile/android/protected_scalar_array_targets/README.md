# Protected primitive-array targets with public erased bridges

Eight final owned classes specialize one protected generic superclass slot for
every primitive array. Only their original erased bridge access is encoded from
protected to public; Code, descriptors and all other metadata stay unchanged.
The typed method remains protected in the input and updates observable state
before returning the original array length or throwing the original exception.

The immutable JVM harness exercises parent callbacks, an original caller, typed
wrappers, reflection and MethodHandles. It checks casts before target effects,
null ordering, mutable aliases, repeated updates and exact exception identity.
Rebuilt typed methods become public as explicitly reported; the public erased
bridges, all runtime descriptors and remaining metadata are retained. The same
oracle runs with the superclass supplied as an immutable declaration JAR.
Same-Code missing flags and changed array bindings fail; an independently
compiled incompatible array override also fails javac.
