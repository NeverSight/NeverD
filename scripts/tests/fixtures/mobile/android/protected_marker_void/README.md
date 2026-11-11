# Protected interface-bounded void consumer

This authored fixture keeps a protected instance method's single interface-bound
formal beside a nongeneric context parameter. A nongeneric wrapper and an
inherited generic wrapper retain the original virtual call. A subclass override
has distinct observable phases. Inputs include null and concrete marker values;
contexts include null, a separate receiver, and the original receiver itself.

The source and SDK profiles compile every generated Java unit for Java 8 and
verify all original class and method identities, flags, fields, Signatures,
Exceptions and generated helper roles. Each profile runs 144 fresh JVM cases,
with two calls on the same receiver. The observer checks exact argument and
context identity, virtual dispatch, prefix and suffix order, partial stores and
original RuntimeException/Error identity. Fifteen deliberately wrong programs
must fail the observer. Four metadata controls change only a Signature constant,
preserve every Code attribute, and must fail recovery before output is published.
