# Plain input return slots beside SDK scalar getters

An independently authored plain input interface owns a fixed reference getter
and a method-owned generic identity contract. Its original implementation
extends an unchanged SDK widget whose ancestor owns an integer getter. A
constant-pool name edit makes their original JVM names equal without changing
Code, descriptors, flags or Signature attributes. A forwarding source child,
a virtual override and separate direct, inherited, interface and SDK callers
retain every original operation.

Run `test_mobile_android_plain_sdk_return_slots.py`. The immutable harness uses
the reported source alias for reflection and checks fixed input callers and
unchanged SDK callers. Mutable return aliases, distinct source and scalar state,
independent SDK overrides, method-formal ownership and original failure objects
must match. A same-name unrelated getter and an existing alias-shaped static
method remain unchanged. Full metadata and runtime inventory differ only by
three reported source names; reflection and binary ABI differences remain
explicitly reported as partial coverage.

A getter-formal mutation retains Code and its descriptor but fails NeverD and
independent javac. Original input classes, SDK declarations and harness bytes
remain unchanged throughout recovery and JVM comparison.

Eight input classes and seventeen methods retain their runtime inventory. The
JVM oracle compares 31 values, 205 identities, 126 state observations and 33
ordered failures, with three explicitly disclosed source aliases.

`--inherited-default` adds an independently owned interface callback. Combined
with `--interface-covariance`, it verifies that the complete default graph uses
the existing source names when checking unrelated inherited scalar slots.
`--source-prefix` keeps the intermediate widget in the source input; the scalar
root remains a frozen SDK declaration. Both profiles retain the original
default provider without adding methods to consumers. An immutable subclass,
direct/interface calls, reflection and method handles compare 120 further calls
and 80 expected failures, including mutable arrays, null receivers, exact long
and double bits, ordered effects and original exception identities. A changed
array read must fail that observer. Source metadata and runtime inventories
still differ only by the specifically reported existing projections.
