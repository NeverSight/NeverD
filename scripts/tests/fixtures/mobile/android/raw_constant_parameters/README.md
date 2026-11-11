# Constant field reads on raw generic interface members

Core implements raw Api and returns Token.VALUE without reading its Box or
Context parameters. The runner compiles immutable callers first, then adds only
the original closed parameter Signature; all Code and runtime descriptors stay
unchanged. Independent javac rejects that typed method as a raw Api override.

Run `test_mobile_android_raw_constant_parameters.py`, optionally with
`--dependency-root` to keep Api, Box, Context, Token and State in an immutable
JAR. Eight call routes compare null and typed throwing arguments, mutable result
aliases, singleton initialization, exact first initialization failures and later
erroneous-class failures. Null receivers never initialize Token. The original
and rebuilt callers run in fresh JVMs for each initialization scenario.

Inventory, all class/field metadata, method flags and Exceptions stay unchanged.
Only Core.run's explicitly reported source Signature is erased. Same-Code
method-formal and unknown-scope counterexamples are rejected. Native controls
also reject accessed or overwritten input registers, non-final owners, changed
field flags, unknown fields, incompatible bounds and parameterized class edges.
The original field-read body is retained; coverage stays `partial`.

The source profile retains six classes and eight original methods; the SDK
profile retains one class and two original methods. The oracle
compares 144 values, 296 identities, 369 state observations and 144 failures.
