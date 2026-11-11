# Type token result passed to a generic superclass constructor

A complete `Core extends Builder<Payload>` header specializes the selected
`Builder(Carrier<? extends T>, int)` contract. The constructor checks its
provider, invokes the original bounded token factory with `Leaf.class`, checks
the actual erased result, and passes that object to its superclass. A known
literal header supplies the closed type relation; it predicts no returned
subclass. One case returns an ordinary Carrier despite the Leaf token and
checks the later payload failure after all constructor effects.

Source and SDK profiles compile every generated unit, compare all original
metadata, Exceptions and helper roles, and run 64 frozen JVM cases twice.
Eight Signature-only controls retain Code and eight independently compiled
wrong programs must fail against the same caller. Original Java 25 classes
are preserved and executed separately from D8 copies that change only the
class-file version header. Generated source also compiles as Java 25. No
private application code executes.
