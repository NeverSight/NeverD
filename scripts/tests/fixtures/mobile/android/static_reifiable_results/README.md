# Static factories with an independent generic result

These independently authored classes use a public static method whose only
Object-bounded formal occurs in its result. Fixed inputs include a callback,
a wide integer and an existing `Class<?>[]`; two overloads retain their varargs
flags and a third method takes an ordinary array. No input determines the result.

The immutable JVM observer checks seven direct, typed, ignored, forwarded and
reflective routes, eighteen input/callback cases and two successive attempts.
It checks actual result and array identities, callback mutations, partial effects,
wide values, exception identity and the caller's original late result cast.
Source and unchanged dependency-JAR profiles compare every original class,
field, method, Signature, role and Exceptions declaration. Independently compiled
wrong programs must fail the observer; Signature-only controls preserve Code.
