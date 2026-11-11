# Original raw interfaces with independent method formals

The authored final `Core` keeps its original raw `Api` edge and Object
superclass, without a class Signature. `Api<E>` extends `Parent<E>`, whose
abstract array member owns two independent method formals. The original
`Core.array` metadata alpha-renames those same complete formals; its Code,
parameters and result descriptor stay unchanged. The encoding helper adds only
that method Signature after the immutable harness has compiled.

The verifier checks source and immutable interface-JAR roots. Recovery erases
only the source method formals under `raw-interface-method-formal-erasure`,
after comparing every bound, parameter and result across the complete known
interface graph. It preserves original declarations and runtime bodies. The
original nongeneric `observe` member supplies shared effects for the unchanged
method-generic default; `entry` calls the projected original array member.

Nine direct, interface, reflected, MethodHandle, default and original-caller
routes compare 45 values, 432 identities, 432 states and 18 failures, including
mutable tag aliases, covariant arrays, nulls and exact exception identity.
Complete class/field/method metadata and inventory permit only the disclosed
array method Signature erasure and reported private helpers. Source and SDK
profiles retain three classes/seven methods and one class/four methods.
Wrong result/tag scopes with unchanged Code and descriptor fail recovery; an
independently authored wrong result scope fails javac. Native cases additionally
reject incomplete bounds/scopes, unavailable ancestors and invalid flags.
