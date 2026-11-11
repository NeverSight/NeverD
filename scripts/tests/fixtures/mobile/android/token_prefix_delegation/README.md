# Class token result in constructor delegation

An independently authored named static child calls a final public instance
`<T extends Carrier<?>> T get(Class<T>)` with an original `Leaf.class` literal,
then passes the actual result into `this(Carrier<? extends Child>)`. The full
`Leaf extends Carrier<Child>` declaration proves source containment. Runtime
erasures stay identical. The provider deliberately returns an ordinary Carrier,
so adding a Leaf cast must fail against the frozen original caller.

Original Java 8 input stays immutable. Recovered prefix locals require Java 25;
that source level and rebuilt class-file version are disclosed explicitly.
Source and SDK modes compile every generated unit and verify all original
identities, class/member metadata, Exceptions and generated helper roles.
Each profile runs 64 fresh JVM cases twice, including null/throwing providers,
partial child state, callback and Error identity, result mutation, late field
casts, and post-delegation effects. Signature controls retain original Code;
independently compiled wrong programs must be caught by the frozen caller.
Only authored fixture code executes.
