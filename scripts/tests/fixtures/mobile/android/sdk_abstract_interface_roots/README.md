# Abstract generic SDK interface roots

Owned `QueueView<E, F>` extends the unchanged standard-library `PriorityQueue<E>`.
It retains a plain own `offer(E)` override that counts calls and invokes its
original SDK superclass. SDK abstract ancestors may leave Queue obligations to
that source override or a nearer concrete SDK provider. The common full member
queue must still prove every composed argument and the actual implementation.

The runner compiles original Java, runs an immutable harness, passes through D8
and NeverD, recompiles recovered source and runs the same harness. Complete
original class, field and method metadata, entire inventory and SDK bytes are
compared. No bridge or Signature projection is expected.

The harness checks nine values, 12 identities, 15 states and six null failures
across direct, Queue interface and reflective paths. It verifies priority order,
element identity, inherited iterator removal, nearest SDK method ownership,
mutable calls and null argument/receiver ordering.

A negative classfile changes only the parameter Signature from class formal E
to equally erased formal F, preserving every Code attribute and descriptor.
NeverD rejects that different complete contract. An independently authored
source mutation must also fail javac; negative inputs are never executed.

```sh
python scripts/test_mobile_android_sdk_abstract_interface_roots.py \
  --neverd build-mobile/bin/neverd --d8 "$NEVERD_D8" \
  --android-jar "$ANDROID_HOME/platforms/android-35/android.jar" \
  --work-dir "$RUNNER_TEMP/android-sdk-abstract-interface-roots"
```

Use JDK 21 and a fresh work directory.
