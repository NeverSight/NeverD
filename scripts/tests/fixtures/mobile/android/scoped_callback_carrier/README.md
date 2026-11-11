# Distinct class and method formals in a callback factory

Genuine Kotlin emits a final package-private Core<T>, a method-owned R,
a Function1<? super T, ? extends R>, and an invariant Box<R> result. The scalar
default produces an unchanged raw synthetic static wrapper with no Signature.
The original wrapper, callback and result are never replaced with adapters.
Parameter and call assertions are disabled explicitly in this owned profile.

The frozen observer exercises reflection and MethodHandles on both original
entries, all low mask combinations and ignored bits, null receivers/callbacks,
opaque identities, callback effects, leaked results and exact failures.
All generated Java units are compiled. Full compiler metadata is checked except
explicitly recorded D8 CLASS annotations, Kotlin/Java source filenames, source
helper methods and synthetic wrapper flags.
