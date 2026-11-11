# Source default parameter bridges

This independently authored fixture retains `Ordered extends Ordering<Ordered>`
and the original default `compare(Ordered)`. The compiler's original public,
synthetic bridge `compare(Object)` casts its argument and invokes the typed
interface method. An empty `Facade extends Ordered` adds a plain intermediate
interface. `Receiver implements Facade` implements only `order()` and inherits
both comparison entries. Neither `Facade` nor `Receiver` has a class Signature;
the proof must traverse both plain edges to the fixed generic specialization.
`Ordering` can be supplied as source or as an immutable declaration JAR with
`--dependency-root`. The source profile contains five classes and seven methods;
the dependency profile contains four classes and six methods.

Run `scripts/test_mobile_android_source_default_parameter_bridges.py` with JDK21,
`--neverd`, `--java-home`, `--d8`, `--android-jar` and a fresh `--work-dir`. Run it
again with `--dependency-root` and a different work directory. The runner compiles
the fixture and independent harness before recovery, compares the complete DEX
and rebuilt inventories, and executes that same immutable harness against both
original and rebuilt classes. The rebuilt runtime classpath contains no original
fixture classes. Original classes, harness, declaration JAR and SDK bytes are
hashed and checked for mutation.

Seven direct, typed interface, generic root, erased root and reflective paths
cover mutable values, both operands' dynamic `order()` overrides, default owner,
argument identity and ordered state. Failures cover null operands and receivers,
default/left/right exception identity, and three erased paths with a wrong-type
argument while the default body is armed to throw. The latter must cast before
any default or operand effect. Expected evidence is 21 value cases, 139 identity
cases, 59 state cases and 38 failure cases. Reflection checks complete root
specialization through the plain intermediate interface, original default and
bridge flags, method formals, Exceptions,
and the absence of invented concrete receiver implementations.

The single original bridge is reported as a `generic-interface-bridge` projection
with its exact typed target and root dispatch. `javac` regenerates that original
entry. Full generic signatures and original method metadata must remain equal;
only explicitly reported source helpers may add methods. Coverage remains
`partial` because that compiler bridge is projected.

Six bytecode-input counterexamples must fail recovery transactionally:

- Replace only the bridge argument load with null.
- Replace only the bridge receiver load with null.
- Change only the full root argument from `Ordered` to `String`, keeping all Code
  attributes and runtime descriptors unchanged.
- Remove only the typed default Code and mark that method abstract while retaining
  its original erased bridge and the concrete receiver.
- Add a more specific abstract typed declaration, explicitly remove any compiler
  bridge from that child interface, and redirect the receiver from `Facade` to
  the child.
- Add a second unrelated interface with its own typed default and compiler bridge
  alongside `Facade` on the concrete receiver.

Each edit writes a separate receipt. The last three intentionally change the
described hierarchy or method body availability; they are not called same-Code
mutations. Independently authored Java variants separately require `javac` to
reject all four invalid hierarchy cases. No rejected recovery is counted merely
because the compiler rejected its source.
