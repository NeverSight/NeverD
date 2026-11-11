# Covariant nested inputs and an invariant result

Genuine Kotlin emits a private static generic function and its raw synthetic
accessor when another class calls that function. Its input has two covariant
unary carrier layers; the result keeps the same two declarations invariantly.
The two integer arguments bind no formal. No classfile or DEX is patched.
Parameter and call assertions are explicitly disabled in this owned profile.

The frozen observer uses opaque lists whose inspection methods fail. It checks
nulls, original/replacement references, integer boundaries, provider effects,
exact failures and the original post-provider List cast through private,
synthetic and instance entries. Every emitted Java unit is compiled. Complete
metadata is compared except explicit source helpers, synthetic accessor flags,
Kotlin/Java source filenames and inventoried D8 CLASS annotations.
