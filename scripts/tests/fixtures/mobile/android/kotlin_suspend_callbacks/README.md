# Suspend callback scope witness

The owned package-part method has a suspend callback parameter. Its frozen
Kotlin 2.0.21 metadata encodes the runtime Function2 classifier, the suspend
type flag, a Continuation argument and nullable Any return. The shared compiler
reader independently checks that tail and the exact enclosing JVM descriptor.
Source and compiler hashes accompany the receipt.

Independent Java bodies use the same erased Function2 contract with a null
continuation. Only the enclosing method Signature and synthetic flag change;
method bytecode stays intact. The JVM oracle checks nineteen action calls,
twenty successful watcher calls, four action exceptions, four watcher exceptions,
captured context/element identity, returned references, zero/negative counts and
three null failures. An action failure must occur before either captured field
is stored; a watcher failure occurs after those stores. Generic reflection
changes are separately verified, and runtime descriptors are retained after
the reported local class rename.

This fixture verifies erased callback dispatch and declaration scope. It does
not reconstruct or execute an asynchronous coroutine scheduler. The external
Function2 and coroutine declarations are independently authored nominal test
contracts. SDK dexdump supplies the complete input declaration inventory.

Run `scripts/test_mobile_android_kotlin_scopes.py --suspend-callback` with JDK 25
and D8. Optional `--kotlin-lib` rechecks the metadata with the compiler reader.
Regenerate the receipt as for the primitive package-part fixture, passing
`suspend` to `InspectKotlinUnitPart`.
