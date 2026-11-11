# Inherited generic providers with empty exceptions

An owned generic interface declares two fixed checked exceptions. An abstract
generic parent supplies the original implementation with an empty exception
list. A forwarding generic middle class, an open child and a fixed Object child
inherit that exact method without redeclaring it.

The independent JVM harness checks direct, typed, raw and reflective calls,
virtual callbacks, argument and result identities, mutable effects, original
exception objects and null receivers. Complete class and method metadata,
including both original exception declarations, must survive recompilation.
Same-Code parameter, result and non-Throwable mutations are rejected, as are
independently compiled incompatible generic and checked-exception overrides.
