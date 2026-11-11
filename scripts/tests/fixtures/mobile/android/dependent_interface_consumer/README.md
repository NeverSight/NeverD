# A parameterized interface upper bound with independent value formals

Genuine Java declares three distinct method formals: one interface-bounded
mapper and two Object-bounded values referenced by that bound. A direct value,
the original mapper subtype and a fixed nominal context remain separate inputs.
The result stays Object. No Signature, classfile or DEX is patched.

The observer checks reflection, MethodHandles and an immutable caller, including
nulls, subtype identity, mutable payloads, repeated calls and exact exceptions.
A real generic bridge in a stricter mapper proves that its original checked
cast still occurs after provider effects. Every generated Java unit is compiled
and complete compiler metadata is compared except reported source helpers.
