# Public erased entries for closed generic protected results

Host owns a static anonymous Base<Fn<? super Throwable, ? extends Throwable>>.
Its protected compute(Class<?>) implementation returns Fn<Throwable,Throwable>
with the complete original method Signature. The runner encodes public static
final anonymous metadata and changes only the original erased bridge visibility.
All original Code, descriptors and Signatures remain unchanged.

Run `test_mobile_android_protected_generic_results.py`, optionally with
`--dependency-parent` to keep Base in an immutable JAR. Six call routes compare
nullable returned functions, mutable aliases, callback effects and failures,
null keys and receivers, method access and full generic reflection. The immutable
harness supplies function implementations outside the input DEX.

Recovery retains the original method/class Signatures and erased bridge. Target
access becomes public and the anonymous class uses the existing named-member
source binding; both changes must be reported. Every other class, field, method
and Exceptions attribute is compared under that binding, as are complete method
inventories. Inputs, immutable callers and dependency/SDK bytes stay unchanged.
Same-Code bridge flags and incompatible generic parameter/result arguments fail
recovery, and independent javac rejects the wrong generic result. Native tests
cover named and anonymous owners, source/SDK parents, redundant result casts,
class/method order and rejection of nonfinal, open or method-owned contracts.

Each profile checks 36 values, 540 identities, 366 state observations and 132
ordered failures. The source and parent-JAR profiles retain five classes/ten
methods and four classes/seven methods respectively, with exactly four reported
method projections for target access, the original bridge and anonymous
construction. Generic Signature projection is not needed.
