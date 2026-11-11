# Interface defaults through captured sibling ancestors

This owned fixture extends the captured sibling oracle with one original
`Cursor<E>.visit(Consumer<? super E>)` default. Both named members keep their
original outer captures, and the child inherits all parent slots and this unique
interface default. Source ownership/Signature changes are reported exactly as
in the companion fixture. No forwarding method is synthesized.

The immutable companion harness still compares 70 values, 133 identities,
147 states and 35 failures. Another independent seven-route harness adds
42 values, 63 identities, 56 states and 28 failures, checking effects before
callback/null/inherited failures, original exception identities and shared
mutable outer state. Source and immutable interface JAR profiles preserve
five classes/fifteen methods or three classes/ten methods, full runtime metadata
except disclosed source scope differences, and all original hashes. Native
controls reject unrelated default competition, abstract shadows, nearer class
slots, raw or different outer bindings and inconsistent interface contracts.

```sh
python scripts/test_mobile_android_captured_sibling_ancestors.py --inherited-default \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/captured-default-source
python scripts/test_mobile_android_captured_sibling_ancestors.py --inherited-default --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/captured-default-dependency
```

Coverage remains partial, and original constructor capture order requires
Java 25. No application artifact or private identity is included.
