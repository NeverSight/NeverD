# Missing Unit-returning package-part scope

The owned `ScopePart.kt` declaration uses `JvmMultifileClass` and an ordinary
generic extension method returning non-null Unit. Kotlin 2.0.21 supplies the
frozen kind-5 metadata, exact facade name and JVM method descriptor in
`metadata.json`. The receipt includes source and compiler JAR hashes.
`InspectKotlinUnitPart.java` independently checks the declaration using the
compiler's own metadata reader. No application data is included.

The execution oracle compiles independent Java bodies with that annotation,
removes only the method Signature and sets its synthetic bit. Method bytecode
remains intact. The anonymous callback retains the original method variable
in its interfaces, fields and constructor metadata. SDK dexdump checks the
complete post-D8 declaration inventory. Recompilation and JVM execution compare
captured references, repeated values, store-before-callback ordering, exception
identity, null failures, void return and original runtime descriptors. Generic
reflection differences are separately verified and reported.

Run `scripts/test_mobile_android_kotlin_scopes.py --unit-package-part` with the
existing JDK 25 and D8 tools. Optional `--kotlin-lib` rechecks the receipt with
`kotlin-compiler.jar` and `kotlin-stdlib.jar`. To regenerate it, compile the owned
Target and Watcher declarations, compile `ScopePart.kt` targeting JVM 1.8, then
run `InspectKotlinUnitPart` and export its length-prefixed metadata output.

NeverD independently reads the bounded wire format from Kotlin's
[metadata schema](https://github.com/JetBrains/kotlin/blob/v2.1.0/core/metadata/src/metadata.proto)
and [JVM schema](https://github.com/JetBrains/kotlin/blob/v2.1.0/core/metadata.jvm/src/jvm_metadata.proto).
Facade kind definitions and the context of `xs` follow the
[metadata annotation](https://github.com/JetBrains/kotlin/blob/v2.1.0/libraries/stdlib/jvm/runtime/kotlin/Metadata.kt).
The production engine does not require or invoke the compiler.
