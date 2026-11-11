# Captured member intersection arguments

An owned private generic member extends a closed generic superclass implementing
two interfaces. Its enclosing class allocates it with ordered virtual getter,
field and array arguments, then passes that exact object to an intersection
method. Independent callers exercise later subclasses, nulls, mutation, partial
constructor/call effects and exact throwable identity. A superclass constructor
reads the original captured outer before the member initializer completes.

The existing captured-member source representation explicitly retains the outer
constructor argument and field, discloses the static member reflection bit and
the capture field synthetic bit, and requires Java 25 for pre-super writes. The
fixture freezes these existing differences before recovery; every other original
identity, method Code role, access, generic signature and exception is compared.

The private constructor's original four unnamed MethodParameters records (the
first final/synthetic, the remaining three unflagged) become an absent attribute
under that existing explicit-outer representation. The fixture requires this
exact original and compiled pair and the established implicit-parameter disclosure;
no other method or parameter metadata receives an exception.
