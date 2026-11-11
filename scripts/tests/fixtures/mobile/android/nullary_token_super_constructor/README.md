# Nullary token superclass constructor

Independently authored Java 8 keeps a private no-argument child constructor,
two independent closed superclass slots and an original `Class<B>` token beside
a string argument. The parent publishes partial state, calls an external
observer and returns before the child stores an opaque value.

Four frozen caller routes compare parent/child effects and exact failures twice.
A missing token class fails at the original resolution point; loading its class
literal must not initialize it. Source and SDK layouts compile every emitted
unit and compare all original entries, Code presence, access, Signature,
Exceptions and MethodParameters. Wrong programs and same-Code metadata controls
exercise the observer and scope binding independently. No private application
code or identifying information is included.
