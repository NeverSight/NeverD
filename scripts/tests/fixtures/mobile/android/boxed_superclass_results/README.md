# Indexed scalar factories through a generic superclass

The authored final `Core` extends `Parent<Object>`, whose `Base<T>` ancestor
and `Api<T>` interface all declare `get(int): T`. Its original erased entry
calls an effectful indexed long target, then an effectful `Box.box(long)`
factory. The class-file encoder records `Parent<Box>` and original bridge/
synthetic flags without changing any Code, descriptor or caller.

The common validator composes every original superclass/interface declaration,
compares complete parameters and result, and verifies the five-op scalar/factory
body including incoming words. It records `boxed-superclass-result-argument`
and `boxed-generic-superclass-result-bridge`; the source superclass argument
returns to Object, retaining the original runtime method inventory. Inherited
class providers and interface defaults consume the same original contract.

The immutable harness covers nine direct, raw/typed interface, original-caller,
reflection, MethodHandle and inherited-provider routes, plus interface callbacks.
It compares 330 values, 1036 identities, 820 states and 42 failures: wide extrema,
new/cached/null factory results, mutable input arrays, repeated calls, aliases,
target-before-factory effects, bounds/null failures and exact exception identity.
All class, field and method metadata is checked, allowing only the reported
superclass argument, original bridge flags and generated private helpers.
Source and unchanged ancestor-JAR profiles use the same oracle.

Same-Code complete result argument drift and missing original synthetic flags
fail recovery. An independently authored incompatible generic return fails
javac. Native controls cover wide parameters and closed static local scopes,
full ancestor contracts, original receiver/argument words, factory order,
result width, visibility, method scopes, missing methods and competing slots.
