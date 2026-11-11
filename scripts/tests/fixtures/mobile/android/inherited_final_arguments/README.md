# Inherited final argument calls

An independently authored generic class initializes fields and allocates a target
using two calls to an inherited protected final method. Its Object and String
parameters and Object result have no method Signature and consume no owner formal.
The complete Child → Middle → Base class signatures remain unchanged.

Source and declaration-provider profiles compile every generated class and compare
all original class, field and method metadata, method roles and Exceptions. Frozen
Java callers exercise direct, reflection and method-handle construction, reference
identity (including null and aliasing), argument order, receiver identity, partial
field stores, class initialization, exact exception identity and failed initialization
on a second attempt. Signature-only negative controls retain all Code bytes;
independent wrong programs must be rejected by the observer.
