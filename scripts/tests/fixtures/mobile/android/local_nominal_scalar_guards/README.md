# Closed local guards with nominal parent unboxing

This owned anonymous view lives in a static factory and captures mutable trace
state. All complete class and field contracts are closed; its own two method
formals keep independent scope. The boolean and integer Object bridges check
concrete byte/long boxes, cast to their declared parent, perform original
virtual unboxing and call original same-name primitive overloads. Wrong/null
inputs return false/minus-one without evaluating either effectful callee.

Only original final class/inner and bridge flags are encoded; all Code,
Signatures and descriptors stay unchanged. The immutable harness checks six
direct/reflection/MethodHandle routes, mutable box and capture aliases, arrays,
boundary byte/long values and original unbox/target exception identities/order:
840 values, 360 identities, 600 states and 120 failures. Source and interface
JAR profiles preserve seven classes/eighteen methods or six classes/fifteen
methods, with only reported local naming, synthetic capture, implicit constructor
Signature and bridge flag differences. Coverage remains partial and Java 25
preserves original constructor capture order.

Native checks include public class slots, concrete implementation of abstract
parent unboxing, scope/nominal/access failures and wide-word order. Unchanged-Code
method-formal result drift and a missing final flag must fail; independent javac
also rejects the method drift. Original input, interface and harness hashes
remain unchanged. No application artifact or private identity is included.

```sh
python scripts/test_mobile_android_local_nominal_scalar_guards.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/local-nominal-source
python scripts/test_mobile_android_local_nominal_scalar_guards.py --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/local-nominal-dependency
```
