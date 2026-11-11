# Multiparameter boxed superclass results

The independently authored input declares `Parent<E,R,C extends Base<R>>`,
protected abstract result slots, a protected original array provider, and a
parameterized interface present on both the owner and its parent. The original
Object-returning public bridge calls the provider and boxing factory exactly
once. Only the frozen class Signature and original bridge flags are encoded
after javac; its original body comes from the authored Java fixture.

Recovery keeps every original class/interface argument and dependent bound.
One explicitly reported public typed result entry contains the same provider
and factory body; javac regenerates the original erased bridge. Provider access,
original class/field/method Signatures and Exceptions metadata remain checked.
The extra typed declaration and forwarding stack frame are reported.

Source and unchanged dependency profiles each run the same immutable six-route
JVM harness. It compares 172 values, 831 object identities, 171 states and 114
expected failures, including mutable aliases and cached/null factory returns.
The runner independently inventories original methods and expected helpers,
recompiles all output, rejects same-Code parent/interface argument, dependent
bound, bridge-flag and target-access changes, and checks a javac counterexample.
The source profile has seven classes and fifteen original methods; the SDK
profile has four classes and eight original methods.

Native coverage varies the result formal's position among three parameters,
scalar/array widths, public/protected declarations, two boxed result slots,
source/dependency parents, direct/linked entry points and input order.
