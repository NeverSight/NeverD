# Parameter-independent erased terminal consumers

This owned fixture changes only classfile Signatures and one original bridge
flag; all Code and descriptors stay unchanged. Its final implementation has
only the erased `add(Object)` entry plus a differently named scalar operation.
Both independently allocate and throw the original immutable-operation exception
without observing the input. The original class edge is `Api<Box>`; normal
methods keep complete wildcard/scoped contracts and a closed iterator result.
The interface extends an unchanged generic parent and an empty marker remains.

Raw source syntax requires four member Signature erasures as well as the class
edge. These are explicitly reported; original method contracts remain the
validator's authoritative data. Declarations, return expressions and reporting
consume one checked source view. No typed runtime add, cast or scalar read is
invented. Complete closed iterator result and field metadata remain unchanged.

The immutable JVM harness checks direct, interface, reflection and MethodHandle
routes, wrong/null mutation inputs, exception allocation/messages, dormant
failures, arrays and shared aliases, effectful normal methods and ordered failure
identity. It compares 174 values, 282 identities, 426 states and 102 failures.
Full metadata and runtime inventories must match except the disclosed raw class
edge, four member Signatures, bridge flags and private source helpers.

The source profile retains five classes/16 methods; an immutable interface and
ancestor JAR profile retains three classes/10 methods. Same-Code full bulk
parameter drift and a missing synthetic flag are rejected. Independent javac
also rejects the bulk contract drift. Input, harness and dependency hashes must
remain unchanged. Every example is owned and contains no application artifacts.

```sh
python scripts/test_mobile_android_erased_terminal_consumers.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/erased-terminal-source
python scripts/test_mobile_android_erased_terminal_consumers.py --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/erased-terminal-dependency
```
