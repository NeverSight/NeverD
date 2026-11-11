The untouched Java 8 originals bind one method formal through a unary producer,
both inputs of a binary callback, and an invariant unary result. The callback
produces a closed nominal Boolean. Its result and the provider's returned carrier
remain opaque to the caller; callbacks may replace that carrier after both input
reads. The erased Client calls the exact original generic method.

Frozen interface, precompiled, reflective and method-handle callers independently
check read/callback order and counts, partial publication, nulls, mutable aliases,
callback cast failures, Boolean unboxing, returned identity and later caller
casts. Every throwing stage, Error identity and sticky initialization failure is
observed in fresh JVMs with two attempts. Source/SDK declarations and invariant
or covariant input profiles preserve all original entries, complete metadata and
Exceptions after compiling every recovered Java file. Independently compiled
wrong programs must fail the same immutable observer.

The key-selector variant is authored from these original sources before any
compiler runs. It replaces the binary callback with a unary callback whose
independent method formal is the opaque key. The input and result keep the
original input formal. A non-Boolean key is published and reaches an explicit
late provider cast, so binding cannot introduce a cast at the callback result.
Both input reads and the actual returned carrier remain observable even though
the selector receives only the first input. All original and recovered class
and method contracts are compared with the separate frozen selector observer.

The unary-callback variant retains the fixed Boolean completion and the same
input/result formal, while its callback receives only the first observed value.
Both input reads, partial publication and callback replacement of the opaque
returned carrier remain observable. It is authored before compilation, then the
original classfiles and callers are frozen. Source/SDK and invariant/covariant
profiles preserve complete Code, access, Signature, Exceptions and parameter
metadata. The former binary callback and independent key-selector profiles
remain separate controls for their original relations.
