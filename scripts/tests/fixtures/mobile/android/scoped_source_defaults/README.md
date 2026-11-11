# Class-owned source default results

Owned `Source<S>` extends `Api<S>` and `Box<S>`. Its original `getBox()` default
retains the complete `Box<S>` result, invokes the receiver callback, then returns
the receiver itself. `Receiver<R>` implements that source interface without
copying the default method. The dependency profile moves only Api and Box into
an immutable JAR, so the original generic source default must prove its complete
SDK ancestor/result relationship.

Both profiles run original javac classes, D8, NeverD, recovered-source javac and
the same immutable JVM harness. Complete original class, method and field
metadata, formal ownership, default reflection, inventory and dependency bytes
are checked. No bridge or Signature projection is expected. The harness compares
10 values, 10 identities, 15 states and five null failures through direct,
source interface, generic and raw API, and reflective paths.

A negative classfile changes only the original default result Signature from
Box<S> to Box<String> while all method identities and Code attributes remain
byte-identical. NeverD must reject it. An independently authored source change
must also fail javac; negative inputs are never executed.

```sh
python scripts/test_mobile_android_scoped_source_defaults.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-scoped-source-defaults"
```

Use JDK 21 and repeat with `--dependency-interfaces` in a fresh work directory.
