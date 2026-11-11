# Typed constructor forwarding with a string literal context

Genuine Kotlin preserves both constructor Signatures on a final generic class.
The short constructor forwards its original typed input and two integers to
the primary constructor, adding one String literal in a closed context slot.
No bytecode, Signature, access flag, literal or DEX record is patched.
Parameter and call assertions are explicitly disabled in this owned profile.

The frozen observer exercises both constructors through reflection and
MethodHandles. It checks object and string identity, integer boundaries,
partially initialized final fields, pre/post-delegation effects and exact
failures. Every emitted Java unit is compiled and complete compiler metadata
is compared, with explicitly inventoried Kotlin/Java source filenames,
D8 CLASS annotations and reported source helpers.
