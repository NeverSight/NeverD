# Named scalar getter bridges

This independently authored fixture encodes four original bridge/synthetic
flags on otherwise unchanged classfile methods. Each method calls a differently
named zero-argument getter and returns its int, long, float or double result.
Code, descriptors and all Signatures are unchanged. The final owner implements
`Api<String>` and an empty marker, with a separate method-owned `<R, S>` contract.

The getters retain effects, live mutable fields, array aliases and preset
exception identities. The immutable JVM harness exercises direct, typed/raw
interface, reflected and MethodHandle routes, floating raw bits, wide extrema,
state changes and null receivers. It compares 401 values, 1,991 identities,
1,272 states and 48 failures. Complete metadata and runtime inventories must
match apart from disclosed bridge flags and private source helpers.

The source profile retains three classes/16 methods; an immutable interface-JAR
profile retains two classes/11 methods. The runner independently verifies all
four exact three-operation DEX bridges. Same-Code method-formal contract drift
and a missing original synthetic flag are rejected; independent javac also
rejects the full contract drift. Input, harness and dependency hashes must stay
unchanged. All examples are owned and contain no application artifacts.

```sh
python scripts/test_mobile_android_named_scalar_getters.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/named-scalar-source
python scripts/test_mobile_android_named_scalar_getters.py --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/named-scalar-dependency
```
