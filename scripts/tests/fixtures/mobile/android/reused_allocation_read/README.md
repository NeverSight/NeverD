# Owned shared allocation producers

A constructor reuses one string-constant value or one volatile field-read value.
A second field's class initializer changes the volatile field between argument
positions, so rereading it changes the constructor's values deterministically.
Target class initialization must precede that argument initializer. Four frozen
routes run 96 fresh JVM cases twice, checking identities, nulls, class-init order,
constructor effects, partial state, fresh results and exact failures. Source
and SDK-target profiles compile all output and preserve original metadata and
Exceptions. Six independent wrong programs guard actual behavior; native cases
cover immutable values, source/SDK declarations and invalid regions/frames.
Only owned fixture and standard SDK code executes. Use JDK 25; originals use
release 8 and recovery uses its reported Java release.
