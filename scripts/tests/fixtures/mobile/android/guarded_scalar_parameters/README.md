# Guarded scalar parameter bridges

This independently authored fixture changes only original bridge flags on two
final methods. Each unchanged `(Object) -> boolean` body tests the input type,
returns false for null or a wrong type, casts, calls the original unbox method
and forwards the long/double result to its original scalar target. Code,
descriptors and Signatures remain unchanged. The owner keeps `Api<String>`, an
empty marker and an independently method-owned `<R, S>` echo contract.

The immutable JVM harness checks direct, typed/raw interface, reflection and
MethodHandle dispatch. It exercises wide extrema, floating raw bits, mutable
fields, shared array aliases, false inputs without effects and both unbox and
target exception identities/order. It compares 699 values, 1,239 identities,
1,980 states and 48 failures. Reflection and complete runtime inventories must
match apart from disclosed bridge flags and private source helpers.

The source profile retains five classes/14 methods; the immutable interface-JAR
profile retains four classes/11 methods. The runner independently confirms both
original 10/11-operation DEX bridges. Same-Code method-formal drift and a missing
final flag are rejected; independent javac also rejects the full contract drift.
Original input, harness and dependency hashes stay unchanged. All examples are
owned and contain no application artifacts.

```sh
python scripts/test_mobile_android_guarded_scalar_parameters.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/guarded-scalar-source
python scripts/test_mobile_android_guarded_scalar_parameters.py --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/guarded-scalar-dependency
```
