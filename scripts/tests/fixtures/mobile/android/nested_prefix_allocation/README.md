# Nested allocations before constructor delegation

This owned program creates a wrapper and an inner value before an original
this(...) or super(...) call. The outer allocation and class initialization must
precede the inner allocation even though its constructor runs afterward.

Four frozen constructor invocation paths run 192 fresh JVM cases twice. They
check class initialization order, erroneous-class reuse, checked/unchecked
exception identity, partial escaped objects, final fields, argument identity,
mutable inputs and the initialized constructor continuation. Fifteen independent
wrong programs qualify the observers, including an inner-first construction with
the same constructor calls. Source and SDK paths compile all emitted sources and
retain every original class/method definition, role, Signature and Exceptions
attribute. The methodless value interface remains part of the class inventory.
