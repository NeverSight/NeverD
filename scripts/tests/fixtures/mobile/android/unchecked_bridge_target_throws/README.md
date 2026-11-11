# Original bridges with typed unchecked Exceptions

Owned Java inputs implement `Api<CharSequence, byte[]>` with a typed method that
declares one RuntimeException descendant and one Error descendant. javac emits
the original erased Object bridge with the interface's empty Exceptions
attribute. The two declarations must be retained independently.

The source and `--dependency-interface` profiles compile the original fixture,
run a separately compiled immutable harness, pass through D8 and NeverD,
recompile the recovered source, and run that same harness. The API JAR and SDK
bytes remain unchanged. All original classes, methods, fields, flags, Signatures,
and exception declaration order are compared exactly; only reported source
helpers may add methods.

The harness checks 10 values, 20 identities, 27 states and 17 ordered failures
across direct, generic interface, raw interface, typed reflection and erased
reflection calls. It confirms retained array and exception identity, null
results/receivers, and wrong-type bridge casts before target effects. The single
original bridge is regenerated and reported as `partial`; no generic Signature
or class binding is projected.

A constant-pool ancestor change replaces RuntimeException with Exception while
all method identities and Code attributes remain byte-identical. NeverD rejects
the resulting checked override. An independently authored source change must
also fail javac; negative inputs are never executed.

Run with JDK 21, D8, Android 35 declarations and a built NeverD executable:

```sh
python scripts/test_mobile_android_unchecked_bridge_target_throws.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-unchecked-bridge-target-throws"
```

Repeat with `--dependency-interface` and a fresh work directory.

The `--default-override` profile authors a plain void default in Api and an
original Codec override that declares the same known unchecked exception types.
A compiled immutable harness exercises direct, typed/raw interface, typed
reflection and interface reflection calls. The first and second invocations
retain their distinct exception identities and ordered effects; a null receiver
performs no body effect. This profile checks 10 values, 30 identities, 42 states
and 32 failures including the original bridge cases. Full original metadata
and inventory remain exact. Repeat with `--dependency-interface` to keep the
original interface/default declaration in the unchanged JAR.
