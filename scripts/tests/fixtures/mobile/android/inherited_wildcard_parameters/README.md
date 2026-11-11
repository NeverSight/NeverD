# Inherited wildcard parameter source erasure

The authored final `Core<K,V>` extends
`Parent<Pair<K,V>,K,V>`, whose first formal is bounded by
`Pair<? extends K,? extends V>`. The parent's public bridge takes `Pair<?,?>`,
while the original child's same runtime slot takes `Pair<K,V>`. Class-file
encoding changes only authored method names and parent bridge flags, preserving
all Code, descriptors and complete Signatures.

The shared validator compares the child's full parameter to the actual
superclass argument and checks the complete substituted interface bound before
recording `inherited-wildcard-parameter-erasure`. Only the child's source method
Signature is erased. Full original contracts, class bindings, runtime slots,
guards and original caller bodies remain intact. Source parent bridge flags
are separately reported through the existing recovery proof.

The immutable harness exercises eight direct, original-caller, reflected,
MethodHandle and inherited-guard routes. It compares 70 values, 348 identities,
740 states and 38 failures, including repeated getter effects, aliasing,
mutation order, nulls, rejected guard inputs and exact exception identity.
Complete class, field and method metadata allow only reported differences and
private helpers. Source and immutable ancestor-JAR profiles retain six
classes/16 methods and three classes/eight methods respectively.

Same-Code wrong child parameter scope and superclass argument binding fail
recovery. An independently authored non-bridge wildcard parent and incompatible
child fail javac. Native controls reject lost bounds, wildcard drift, missing
bridge roles, static/private entries, method formals, throws and unsupported
instructions; both source and immutable dependency ancestors use the same proof.
