# Static argument results in superclass construction

This independent fixture uses a descriptor-only static factory with a class-literal
argument before invoking a generic superclass constructor. The required carrier
contains a completely closed nested wildcard type. The factory deliberately returns
an independently supplied raw object: the class literal supplies no result binding.

Both source and SDK-declaration profiles retain original class, field and method
metadata, method roles and Exceptions, and compile every recovered source. Frozen
callers check class initialization, actual result and token identity, generic input
identity, partial parent/child stores and exact failures across two attempts. A
class literal must not run its referenced class initializer. Signature-only negative
controls retain all original Code. Independently changed programs must fail the
observer. Java 25 constructor-prefix inputs have a separately verified, header-only
class version change for D8, with both versions executed before recovery.
