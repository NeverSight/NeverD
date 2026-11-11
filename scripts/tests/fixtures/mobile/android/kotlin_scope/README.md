# Missing synthetic method scope

`MetadataHost.kt` is an independently authored declaration. Kotlin
2.0.21-release-482 produced the annotation in `metadata.json`; the receipt
records the source and compiler JAR hashes. `InspectKotlinScope.java` uses the
compiler's own metadata reader to check the exact JVM method identity and the
two unconstrained formals. No application data is included.

The execution oracle compiles the Java fixtures with this annotation, removes
only `adapt`'s `Signature`, and sets its synthetic access bit. Its original
method bytecode remains unchanged. The anonymous class still references the
missing formals in its own metadata. D8 supplies the actual binary input to
NeverD. The JVM checks captured identities, repeated calls, exceptions, null
values, and the explicitly changed generic reflection. Recompilation checks
original descriptors after the reported local class rename.

CI uses the frozen compiler receipt and needs only the existing JDK 25 and D8
test tools. For an independent metadata-reader replay, pass `--kotlin-lib` with
the directory containing `kotlin-compiler.jar` and `kotlin-stdlib.jar`. To
regenerate the receipt, compile the owned Box/Callback/Watcher declarations,
compile `MetadataHost.kt` with Kotlin targeting JVM 1.8, then run
`InspectKotlinScope` against those classes and export its length-prefixed output
as the receipt's annotation values. Recheck the source and compiler hashes.

NeverD's reader independently implements a bounded subset of the wire format
specified by Kotlin's [metadata schema](https://github.com/JetBrains/kotlin/blob/v2.1.0/core/metadata/src/metadata.proto)
and [JVM schema](https://github.com/JetBrains/kotlin/blob/v2.1.0/core/metadata.jvm/src/jvm_metadata.proto).
It does not embed or invoke the Kotlin compiler.
