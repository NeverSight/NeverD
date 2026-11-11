# Bounded formals, Object contracts and fixed defaults

This independent fixture combines four requirements on the same class ancestor.
`Parent<T extends Bound>` owns a mutable `T` field, a `T get()` method and a
`T echo(Callback<String,byte[]>)` method. `Bound` is a known plain interface. The
original method and field descriptors erase to `Bound`; their original generic
Signatures retain class-owned `T`. `Child extends Parent<Token>` declares no
implementation beyond its constructor.

The plain source `Source` interface declares the `Bound` results and extends
`ObjectView`, `OrderNode` and `TypeView`. `ObjectView` declares `equals`, `hashCode` and
`toString`; the class hierarchy satisfies them through the unchanged original
`java.lang.Object` entries. `OrderNode extends Ordering<OrderNode>` provides a
default comparison that calls both operands' virtual `get().value()` methods.
Its original public synthetic `compare(Object)` bridge casts the argument to
`OrderNode` before invoking the typed default. No getter, Object implementation
or comparison is added to the concrete child. `TypeView.type()` has the complete
closed result `Class<? extends Bound>`. Parent implements that exact result and
returns `Token.class`; the child inherits it without another declaration.
The `--default-type` profile instead supplies that same implementation as a
default on Source and removes Parent's declaration before compiling the owned
input. The harness requires Source to remain the declaring owner through Parent
and Child, with no replacement class method or additional type bridge.

Run `scripts/test_mobile_android_bounded_formal_object_contracts.py` with JDK21,
`--neverd`, `--java-home`, `--d8`, `--android-jar` and a fresh `--work-dir`.
The `--dependency-root` profile moves `Ordering<T>` and `TypeView` to an immutable
JAR; `Source`, `ObjectView` and `OrderNode` remain source declarations. Source
inventory is eleven classes and twenty methods; dependency inventory is nine
classes and eighteen methods. The recovered source compiler uses the report's minimum Java
release. Exactly one original interface bridge is projected with its full typed
target and generic root dispatch; no generic Signature or class binding is
projected. Coverage is `partial` for that compiler-regenerated original bridge.
Run a third profile with `--dependency-root --default-type` to combine the source
default provider with the immutable external TypeView declaration. Class/method
counts and all behavioral case counts remain the same in both provider modes.

The immutable harness is compiled only against original classes, then reused
against recovered classes without original fixture classes on the runtime
classpath. It verifies typed, raw, source interface and reflective getter calls;
mutable source identity and nulls; exact fixed callback types; original bounded
formals; and no invented child implementations. Seven direct, interface and
reflective class-token paths preserve returned Class identity and have no source
or default effects, including with unrelated getters armed to throw. Their null
receiver cases retain failure behavior. Reflection preserves the full wildcard
direction and bound, the plain Class erasure and original declaring owners.
All original method and
constructor Exceptions are checked by exact reflection lookups, independently
of reported generated helpers. Complete class, field and method metadata and
all method identities are compared before execution.

Object identity equality, identity hash codes and original Object string values
are checked through class, parent, both source interface views and reflection,
with unrelated getters armed to throw. Reflection must resolve the inherited
class declarations to `java.lang.Object`. The default comparison covers seven
typed, erased and reflected paths, mutable getter values, null receivers,
operands and sources, wrong-type casts before any body effect, and exact default,
left-getter and right-getter exception identities and order. Expected evidence
is 100 value cases, 596 identity cases, 194 state cases and 94 failure cases.

Four same-Code Signature counterexamples change a class-owned getter return
formal to a method-owned formal, exchange the full fixed callback arguments,
change the fixed default's generic root argument, or change only Parent's
class-token result to `Class<? extends String>`. In `--default-type` mode this
edits Source's original method Signature instead. The last mutation keeps the
`TypeView` declaration and dependency JAR unchanged in both profiles. Three additional owned
variants first compile legal private, static or abstract `same(Object)` methods,
then rename only that Parent declaration to `equals`, preserving all Code,
descriptors, flags and Signatures. The abstract variant leaves the child's
original `same` implementation unchanged so it cannot implement the new abstract
`equals` slot. These original class slots must block inherited Object fallback.
Separate Java variants require javac to reject all seven illegal contracts. The
runner records each input edit, requires NeverD to reject transactionally, and
checks that original fixture, harness, dependency and SDK bytes remain unchanged.
