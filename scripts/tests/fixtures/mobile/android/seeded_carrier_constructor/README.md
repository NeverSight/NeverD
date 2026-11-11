# Seeded generic constructors

Owned source and SDK layouts cover a nongeneric concrete child of a recursive
generic parent, with independently scoped constructor formals and a seed paired
with an invariant unary carrier. The child also has a scalar overload and a
nongeneric overload. A generic caller passes its original receiver and reads a
static carrier after allocation; class initialization replaces that carrier.

The frozen caller and observer run seven entry routes, two callback variants and
sixteen modes, with two attempts in each fresh JVM. They compare receiver and
argument identity, partial initialization, callback count and dispatch, primitive
float bits, completion order, checked/runtime/Error identity, failed class
initialization and its later NoClassDefFoundError. Deliberately wrong programs
qualify the observer before recovery. Eight same-Code Signature controls retain
exact constructor metadata through D8 or an independently built SDK provider.
Every emitted Java unit is compiled and all original identities, method Code
roles, access, Signature, Exceptions and parameter metadata are compared.
