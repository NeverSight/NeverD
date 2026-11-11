# Captured sibling superclass inheritance

This owned fixture has two original nonstatic members capturing one generic
outer instance. The child extends `Owner<T>.Base`, implements another interface
and inherits `hasNext`, `next` and a method-owned array formal named `T`.
The common validator proves both original constructor captures and exact outer
arguments before the disclosed static source projection. No inherited runtime
method is added, and the shadowing method formal remains independent.

The immutable harness compares seven direct, superclass, interface, reflection
and MethodHandle routes, mutable outer state, shared arrays and exception
identities/order: 70 values, 133 identities, 147 states and 35 failures. The
source profile retains five classes/fourteen methods; an unchanged two-interface
JAR profile retains three classes/ten methods. Recompiled inventories include
only original declarations and reported private helpers. Complete metadata must
match except the disclosed static ownership, synthetic capture flags and seven
owner/Object Signature changes. Coverage remains partial and Java 25 preserves
original constructor capture order.

Same-Code owner-bound, owner-substitution and method-result mutations must fail.
Native controls also reject missing captures, own class formals, raw owner edges,
inaccessible/abstract/static nearest slots and shadowed method-bound drift.
Input, dependency and harness hashes remain unchanged. No application artifact
or private identity is included.

```sh
python scripts/test_mobile_android_captured_sibling_ancestors.py \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/captured-sibling-source
python scripts/test_mobile_android_captured_sibling_ancestors.py --dependency-root \
  --neverd build-mobile/bin/neverd --android-jar "$ANDROID_JAR" \
  --d8 "$D8" --java-home "$JAVA_HOME" --work-dir /tmp/captured-sibling-dependency
```
