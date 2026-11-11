# Public erased covariant array results

Core has a protected int-parameter method returning Slot[] and an original
public erased bridge returning Broad[]. Base retains its bounded S[] contract,
Slot retains its complete Broad<Core<?>> ancestry and Child inherits the slots.
The runner compiles an immutable JVM harness before changing only the original
bridge access from protected to public. Code and descriptors remain unchanged.

Run `test_mobile_android_protected_array_results.py`, optionally with
`--dependency-parent` to keep Base in an unchanged JAR. Six routes cover Core,
Child and an independently compiled protected override. The oracle compares
null and cached results, fresh arrays and their zero initialization, mutable
aliases, a normal array upcast body, exact component types, failed element stores, negative lengths,
failure identity and effects before failure: 486 values, 3204 identities,
3456 state checks and 672 ordered failures per profile. Source recovery retains
five classes/eleven methods; the parent-JAR profile retains four classes/eight
methods. Complete metadata changes only at
the reported typed access, with no body copied into Child. Recompiling the
separate protected override against the recovered public target requires public
access; the report discloses this difference.

Same-Code bridge-flag and parent-argument mutations fail recovery, and independent
javac rejects the incompatible result contract. Native tests additionally vary
result casts, self copies, source/SDK declarations and class/method order, and
reject wrong receivers/arguments, nonvirtual forwarding and source overrides.
Coverage remains `partial`.
