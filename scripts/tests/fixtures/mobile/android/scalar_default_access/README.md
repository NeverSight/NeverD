# Scalar interface default accessors

Kotlin 2.0.21 compiles these authored templates with all-compatibility defaults.
Each scalar specializes a generic value getter. The genuine synthetic static
compatibility entry invokes that exact boxed default body, then invokes its
original wrapper unboxing member and returns the primitive. Classfiles and DEX
Code are never rewritten.

The frozen Java observers call DefaultImpls directly and through a separately
compiled caller, reflection and method handles. Each profile runs 112 fresh
JVM cases with two attempts, checks all eight scalar samples, original default
versus overriding dispatch, partial fields, late input mutation, null receivers,
exact RuntimeException/Error identity and ordinary generic-view virtual calls.
Floating samples include signed zero, NaN payloads, infinities and subnormals;
wide integers include both extreme values and distinct high words.

Every generated Java file must compile at the reported release. Original
definitions, class/method metadata and Exceptions are compared; only the
reported synthetic accessor flag and reported private default/helper entries
may differ. Four separately compiled wrong programs change the captured result,
receiver, an effect or compatibility dispatch and must fail the frozen oracle.
