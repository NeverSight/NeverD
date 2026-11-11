# Primitive and coroutine reference package-part scope

This owned package-part declaration combines an Int parameter, an ordinary
CoroutineContext reference, an Object-only method formal and a generic Target
return. Kotlin 2.0.21 generated the frozen metadata and its source/compiler
hash receipt. The shared `InspectKotlinUnitPart` compiler reader independently
checks the exact JVM method, primitive parameter, context classifier and formal.

Independent Java bodies capture the count, context and element in a callback.
Only the enclosing method Signature and synthetic access bit change before
D8. The JVM oracle compares eight forwarding probes, twenty successful callback
calls, four exact callback exceptions, zero/negative counts, null behavior,
store-before-callback ordering, context identity and returned Target identity.
SDK dexdump verifies the complete input inventory; source recompilation retains
runtime descriptors after the reported local class rename. Generic reflection
differences remain explicit. The owned CoroutineContext declaration supplies
only nominal type identity; the fixture does not invoke its library methods.

Run `scripts/test_mobile_android_kotlin_scopes.py --primitive-parameter` with
JDK 25 and D8. Optional `--kotlin-lib` rechecks the receipt using the compiler's
own metadata reader. Regenerate it as described in the Unit package-part
fixture, passing `primitive` to `InspectKotlinUnitPart`.
