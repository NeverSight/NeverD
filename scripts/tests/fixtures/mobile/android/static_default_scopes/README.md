# Static default constructor scope fixture

These independently authored declarations model an inlined synthetic default
method and an anonymous callback. The original generic method and its wrapper
construct the same `Holder<Payload>` from the same unchanged `Token<E>` input.
The wrapper can replace the other constructor argument without changing that
type relationship. The formal names deliberately differ.

The runner removes the wrapper's unused method Signature, sets its synthetic
flag, and changes only its class and field Signature UTF8 records to reference `E`.
It compares every original Code attribute before and after these metadata edits.
No private input or derived method body is included.

The independent JVM harness checks 32 returned object identities, mutable values,
default masks, constructor and read exception identity, and null-failure ordering.
It confirms that the original generic method remains generic, runtime descriptors
remain available, and the callback's unavailable type variable becomes `Object`
with an explicit partial-recovery metadata report.
