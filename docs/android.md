**Languages**: [English](android.md) | [简体中文](zh-CN/android.md) | [繁體中文](zh-TW/android.md) | [日本語](ja/android.md) | [한국어](ko/android.md) | [Français](fr/android.md) | [Deutsch](de/android.md) | [Español](es/android.md) | [Italiano](it/android.md) | [Русский](ru/android.md) | [العربية](ar/android.md)

# Android Java recovery

[← Documentation index](README.md)

`neverd mobile` recovers readable Java from APK, DEX, and smali using NeverD’s built-in engine by default. Independently implemented readers share a typed Dalvik model and a bounded Java emitter. This experimental CLI feature does not claim feature parity with JADX or complete recovery of arbitrary APKs. APK containers and Java output are not exposed through the native C SDK, Python plugin SDK, GUI loader, or `neverd decompile --language`.

Recovered Java is a reconstruction of bytecode. Original comments, formatting, source-language choices, and removed identifiers are unavailable; Kotlin bytecode also produces Java. A successful run does not prove semantic equivalence or guarantee that every method recompiles. No analyzed application is launched by this workflow.

## Quick start

After preparing the runtimes below, choose a new output directory:

```sh
neverd mobile app.apk -o recovered-app
neverd mobile classes.dex -o recovered-dex
neverd mobile MainActivity.smali -o recovered-class
neverd mobile decoded/smali -o recovered-java
```

Open `recovered-app/sources/` to read the Java files and `recovered-app/report.json` to inspect the input inventory and limitations. A directory input is preferable when smali classes refer to each other.

## Runtime setup

| Component | Requirement | Selection |
|-----------|-------------|-----------|
| NeverD | Build the `neverd` target with C++20 support. The mobile workflow is compiled into the native CLI and does not use a Python interpreter. Distribute the executable with the native libraries required by your build. | `build/bin/neverd` / PATH |

The default engine is implemented in C++20 and needs no Python, Java, or JADX runtime. It accepts representable ordinary declarations and operations from DEX 035 and 037–040 and from smali. DEX 041, dynamic calls such as `invoke-custom`, some initialization paths, unknown semantic annotations or operations, and identifiers that Java cannot represent fail explicitly. File-format acceptance does not mean every instruction or declaration in that format is supported.

Java name binding distinguishes class headers from class bodies and can resolve known same-package shadowing; it explicitly rejects cases where missing external superclass or interface declarations prevent it from establishing type or generated Java helper bindings. The platform `java.lang.Object` and `java.lang.annotation.Annotation` contribute no inherited member types. Smali floating-point literals are rounded directly to their target single or double precision, preserving the resulting bit patterns.

Use repeatable `--classpath dependency.jar` options to supply missing Android SDK or library declarations. For example, `--classpath "$ANDROID_HOME/platforms/android-35/android.jar"` supplies SDK types. The built-in classfile reader accepts Java 1.1–17 declarations, including inheritance, member types, signatures, exceptions and annotation element defaults/policies. Dependency JVM bytecode is not executed or decompiled; it is opaque to this declaration reader. No Java runtime is needed. Input definitions override matching dependency definitions; duplicate dependency definitions fail. Missing referenced member classfiles and unproven inherited names still fail. Modular and multi-release JARs require a separate, target-specific declaration set and are currently rejected. Classpath facts do not make other unsupported source shapes recoverable.

The built-in C++ engine preserves and validates supported class, field, and method `Signature` metadata, including type variables, arrays, wildcards, bounds, and method-level shadowing. Erasure must match the original DEX declaration identity. `Throws` is retained; an unproven exception hierarchy is rejected. Direct generic interfaces with concrete type arguments can use a verified bridge projection: each original bridge must perform only ordered parameter checks, one exact virtual delegation and return of its result. The emitter retains each bridge identity in coverage, records the specialized target and erased inherited dispatch, and leaves Java to regenerate the bridge. These rows are `source-projected` with `projection_kind: "generic-interface-bridge"` and keep overall coverage `partial`.

Anonymous classes enclosed by a synthetic static `$default` method can refer to formals absent from that method's metadata. A source projection requires one exact original generic declaration with a matching parameter prefix, integer mask words and terminal Object marker, plus an actual static call to that declaration or construction of its proven shared anonymous body. Only unconstrained Object formals are admitted. Their uses become Object and report `synthetic-default-method-type-variable`, the original signature and exact `scope_origin`; no formal is invented on either method. Own method formals still shadow the enclosing scope. Missing witnesses, mismatched descriptors, unknown variables and additional bounds fail. Generic reflection may differ; coverage remains `partial`.

An inlined static default method can instead witness one Object-bounded formal through an exact shared fresh constructor. Both bodies must forward the same unchanged incoming reference, or its proven entry-prefix copies, to the same parameter of that constructor. The known top-level Object-based class must have one Object-bounded formal; the public constructor must have no own formals. Its complete parameter type must match the original method parameter after substituting that class formal with the original method formal, by declaration ownership rather than name. A changed second input does not invalidate an independently proven first input. Conflicting definitions, overlapping wide writes, handlers, missing allocations and branches into constructor preparation reject the witness. The independently authored D8/JVM oracle preserves all original Code bytes and descriptors, checks 32 returned identities and six ordered failures, and confirms both the retained original method formal and the reported callback reflection differences.

For an inlined instance default helper, the first parameter must be the exact declaring receiver, followed by the original parameters, mask words and Object marker. The wrapper and original must both invoke the same declared zero-argument instance member on that unchanged incoming receiver or its proven entry-prefix object copies. Every later definition of an alias must preserve that receiver; conflicting copies, overlapping wide writes and changed alias sources invalidate the proof independently of branch order. Nested owners, handlers, changed receivers and unknown member identities prevent this witness. Class and method formals must have only Object bounds. The anonymous body's class-formal uses report `synthetic-default-receiver-type-variable`, recording both the original class scope and exact wrapper identity. The javac/D8/JVM oracle preserves original code while comparing callback selection and identity, receiver call counts, mutable values, exceptions and null behavior; it separately confirms the reflection difference. A second D8 oracle requires SDK-confirmed high-register entry copies and retains seventeen live object identities across each receiver call.

A single type-variable bound encoded in the interface-bound slot can use Java's class-bound spelling without changing its constraint, owner or erasure. The `type-variable-bound-position` projection records the changed signature encoding; additional bounds are never dropped. An independent JVM oracle compares class/method bound reflection and reference identity across both branches and null inputs.

A private static synthetic boolean helper can encode `<T: Object, R: Bounded<T> & Iterable<? extends T>>` even though `Bounded<E>` requires `E extends Comparable<? super E>`. The bounded `checked-synthetic-bound-erasure` projection requires the exact two-parameter shape and an original adjacent copy/cast/interface-call sequence from the unchanged incoming references. Backward branches, entry into the middle of that sequence, missing contracts, declared exceptions and enclosed local classes prevent projection. The sole incompatible argument bound is recorded; lexical binding, arity, other bounds and original descriptor erasures still validate. The source uses that runtime descriptor and preserves all original instructions, including the cast before a null receiver failure. The independent javac/D8/JVM oracle checks that order, null short circuits, bridge casts, callback effects and exception identity. Generic reflection differs and coverage remains `partial`.

A separate reading-oriented projection handles the non-Java method shape `<C: Object[] & R, R: Object> R method(C, Callback<? extends R>)`, or a declared interface with only unbounded wildcard arguments in place of `Object[]`. Interface formals must themselves have only Object bounds. It requires a private static body, a declared callback interface with one Object-bounded formal, and no enclosing local class or declared exceptions. Original type arguments and descriptor erasures must validate first. The source then uses the exact runtime parameter/result descriptors without method formals; `array-intersection-method-erasure` or `interface-intersection-method-erasure` retains the complete original Signature and records loss of the compile-time intersection constraint and generic reflection. Other bounds, public or virtual contracts, and captured scopes still fail. The javac/D8/JVM oracles preserve bytecode in their independent baselines and compare reference identity, interface dispatch, callback effects and failures, null ordering, access and descriptors. Coverage remains `partial`.

An anonymous class enclosed by a synthetic lambda implementation can recover a missing Object-only scope through a unique desugared creation chain. The original generic method must construct a stateless synthetic adapter whose complete body forwards the original arguments, including exact reference casts, to that implementation; the implementation must construct the anonymous class with proven captures. Naming alone is insufficient. Missing or competing origins, changed forwarding, unknown variables and additional bounds fail. `desugared-lambda-method-type-variable` records the exact original method and the replacement of its variables by `Object`. Runtime descriptors and bodies remain intact, while generic reflection changes and coverage remains `partial`. An owned javac/D8/JVM fixture checks complete method inventory, captured identity, callback effects and exceptions, and null behavior.

Parameterized bounds can follow declared generic superclass and interface paths with invariant arguments. Actual and required type substitutions remain separate, including nested arguments and several inheritance levels; a subclass of `Base<String>` cannot satisfy a required `Base<Object>`. Required `? extends`, `? super` and unbounded arguments use their respective containment directions, while invariant arguments still require exact types. A wildcard actual argument can also follow an exact invariant formal slot on each nominal inheritance edge, preserving its original upper, lower or unbounded direction. The proof uses a temporary edge instance and leaves declaration plans and source metadata unchanged. Nested or composed wildcard rewrites, raw paths without bindings, conflicting outer-instance formals and excessive/cyclic proofs remain unproven. Identity self bindings stay exact. Each traversed edge must match the raw declaration, including dependency declarations, and unknown variables cannot hide behind a required wildcard. An owned JAR/D8/JVM oracle checks upper, lower and unbounded paths, choices and reference identity, failures, unchanged generic reflection and descriptors; an invariant counterexample is rejected by both NeverD and javac. This validates bounds without authorizing unproven generic member invocation.

For synthetic static overloads without generic metadata, a captured variable may use an exact same-name generic call as its scope witness. The same incoming reference must reach both a proven synthetic capture parameter and the target's corresponding parameter, with identical generic type structure. Every target formal must have that witness and an unconstrained Object bound. Stable copy origins and copies within one basic block are tracked; conflicting values, bypassed copies, handlers, additional bounds and competing origins fail. The report uses `synthetic-call-capture-type-variable` with `scope_origin`. This establishes a declaration scope only; constructor and generic invocation emission still require their own proofs.

An original `kotlin.Metadata` annotation can also witness the missing scope of an anonymous class's exact enclosing synthetic static method. The bounded reader matches the method name, complete descriptor, and Object-only formal bounds; it does not select an overload by name alone. It supports the modern byte encoding in class, file and multi-file package-part metadata versions 1.4–1.9 and 2.0–2.1, ordinary reference descriptors, exact declared variable IDs/names, indexed types, and a narrow string-table subset. A package part requires a valid distinct facade internal name in its own JVM package. The eight primitive classifiers map to their exact JVM descriptors or boxed descriptors when nullable; indexed type-table nullability is retained. Ordinary `kotlin/coroutines/` reference classes keep their JVM names, and the complete method descriptor must still match. Function0–Function22 classifiers map to their runtime `kotlin/jvm/functions/` interfaces. A suspend callback additionally requires the exact Continuation/nullable-Any ABI tail and an explicit, matching enclosing JVM descriptor; this witness does not reconstruct coroutine scheduling or permit a suspend enclosing method. Non-null Unit maps to void only in an ordinary return position; it cannot authorize a parameter, receiver or formal bound. Other encodings, ABI adaptations, additional bounds, unknown variables, malformed or competing exact declarations remain unproven. `kotlin-metadata-method-type-variable` records replacement of those variables by `Object` and the exact `scope_origin`, without inventing formals on the synthetic method. Generic reflection changes and coverage stays `partial`. The fixture uses an owned Kotlin compiler metadata receipt, independently compiled Java bodies with only a Signature and access-flag mutation, D8, recompilation, and JVM behavior checks; it does not require Kotlin at decompilation time.

A generic member can substitute complete parameterized types only when the implementation has the exact complete method Signature. The shared substitution retains each nested argument, wildcard direction, inner segment and array dimension while separately producing its runtime descriptor. Descriptor equality cannot authorize differing generic parameters or results. Unavailable target Signatures, method formals outside the exact Object-bounded profile below, generic throws, unproven fixed exception compatibility and enclosing inherited scopes remain unproven. The javac/D8/JVM oracle retains eight classes and all fifteen method descriptors, compares 40 identities and 22 ordered failures, and checks both compiler bridges and a nested member whose descriptor needs no bridge. Mutable reads and writes, virtual overrides, array casts and generic reflection remain matched. A single target Signature change with identical Code bytes and runtime descriptors is rejected by both NeverD and javac.

Inherited method formals with exactly Object bounds may retain an unchanged runtime slot without declared exceptions or a new bridge. Full substitution assigns each method formal a position-specific equality key, allowing alpha-equivalent names while keeping class and method declaration scopes distinct. Both declarations must have the same formal count, exact bounds, complete parameters and compatible result; erasure alone cannot authorize reordered formals or substitution with a class variable. Bindings are local to each inherited member and cannot change a subsequent class-formal contract. The independently authored interface JAR stays unchanged while its input implementation is recovered, recompiled and executed. The oracle retains all four input descriptors and generic reflection, compares 35 identities and seven ordered failures, and checks array identity, mutable effects and direct/interface calls. A single swapped method-formal Signature with unchanged Code and runtime descriptors is rejected by both NeverD and javac. Coverage is `recovered`; non-Object bounds and changed runtime slots still require separate proofs.

A consumed open argument must have its lexical availability, exact declaration identity, bounds and erasure resolved by the common validator. This includes class formals and an anonymous subclass's exact enclosing method formals. The substitution key retains both owner identity and variable name; matching names and erasures cannot substitute different scopes. The target needs an original method Signature, identical full substituted parameters and a compatible result. Return containment accepts an unbounded wildcard, or an invariant argument equal to the declared upper/lower wildcard bound; it does not guess nominal subtype paths or recursively widen nested invariant arguments. Arrays retain their original dimensions. Concrete superclass overloads with no matching specialized parameter slot remain inherited, including final overloads; an actual final override still fails. An independent javac/D8/JVM oracle retains eight classes and all 24 method descriptors, checks 133 identities and 15 ordered failures, preserves owned formals, bounds and wildcard reflection, and confirms bridge casts, mutable effects and inherited virtual dispatch. A target using a different class formal despite identical Code bytes and runtime descriptors is rejected by NeverD and javac. Regenerating the single original bridge keeps coverage `partial`.

A concrete source method can retain an Object-bounded formal declared by its own
class as its result when implementing a plain Object interface result. The
runtime return descriptor stays Object. One shared result proof serves direct
implementations, plain interface requirements inherited through a source parent
and concrete inherited providers. Every full parameter still has to agree;
method-formal shadows, different complete generic arguments, throws and nearer
incompatible declarations cannot reuse the proof. A required type variable whose
erasure is Object retains its distinct full contract. Source and dependency
interfaces use the same rule, including defaults overridden by the class body.
No bridge, inherited method or generic source projection is added.

The class-formal-object-returns source and `--dependency-interface` JVM profiles
check a getter and a fixed parameterized callback method through typed, class,
interface and reflection views. A closed subclass acquires no getter or callback
override; mutable collection and null results keep their identities,
callback effects and ordered failures. Original class and member metadata,
including the class-owned result Signature, are compared in full. Same-Code
method-formal shadow and swapped callback arguments fail, with independent javac
counterexamples. These profiles use JDK 21 and retain zero source projections.

A plain typed implementation can declare known unchecked exceptions while its
original erased bridge retains the interface's empty Exceptions declaration.
The common validator proves every declared exception's RuntimeException or
Error ancestry; the original forwarding verifier still checks casts, receiver,
delegation and result. The unchecked-bridge-target-throws source and
`--dependency-interface` profiles compile owned javac bridges, retain complete
class and member metadata, and compare returned arrays, ordered effects,
exception identity and casts before effects. A same-Code ancestor mutation from
RuntimeException to Exception is rejected, alongside an independent javac
counterexample. Original typed exception order and empty interface/bridge
exception declarations remain unchanged; bridge regeneration is reported as
`partial`. The additional `--dependency-interface --default-override` profile
keeps an original plain void default override with declared unchecked exceptions.
Direct, typed/raw interface and reflective calls preserve alternating exception
identity and effects, null receiver ordering, the implementation's exception
order and the interface's empty declaration. The same complete metadata and
inventory checks remain in place; no default bridge is generated.

`test_mobile_android_scoped_unchecked_implementations.py` checks class-owned
`G read(G)` and method-owned `<R> R unwrap(Class<R>)` declarations with original
unchecked Exceptions lists. The generic interface declarations have empty
exception contracts. Source and immutable root-JAR profiles retain four
classes/seven methods and three classes/five methods respectively, apart from
reported private helpers, with zero bridges or metadata projections. Each
immutable JVM profile compares 48 values, 536 identities, 104 state observations
and 56 ordered failures through direct, raw interface and reflected calls,
including an independent override, mutable aliases, nulls, original exception
identities and Class casts after effects. All class and method scopes and ordered
exception metadata recompile unchanged. Same-Code method-bound drift fails
NeverD and independent javac. Native controls also reject checked or mixed
exception hierarchies, generic throws, altered full parameters, unknown types,
static drift and missing common closed/unchecked receipts. Coverage is `recovered`.

`test_mobile_android_parameter_return_covariance.py` specializes a generic
parent/interface key to String and narrows the concrete result to its source
implementation class. There is one original checked Object/Object bridge; no
String/Object-to-Object intermediate runtime entry exists. The typed target keeps
its ordered unchecked Exceptions. Source and immutable parent/interface-JAR
profiles retain five classes/eight methods and three classes/five methods, apart
from reported private helpers, and regenerate only the original bridge. Each
immutable JVM profile compares 108 values, 1,224 identities, 198 state observations
and 90 ordered failures through nine direct, typed/raw parent/interface and
reflected routes, including an independent override, mutable array aliases,
nulls, exception identity and key casts before effects. Complete metadata and
runtime entries recompile unchanged. Same-Code full class argument and unchecked
ancestor mutations fail NeverD and independent javac against the authored
original override. Native controls also cover separate class/interface and
source/SDK roots, complete parameterized carriers, missing common covariance or
unchecked receipts, altered invariant arguments and original bridge casts,
receiver/argument origins, flags, invocation or Exceptions attributes. Coverage
is `partial` only for the original bridge; no generic metadata projection is
added.

The `--deprecated-target` source and immutable parent/interface-JAR profiles
of `test_mobile_android_parameter_return_covariance.py` retain a matching original
Deprecated marker on the typed method and its checked compiler bridge. They keep
the same five classes/eight methods and three classes/five methods, one original
bridge and 108 value/1,224 identity/198 state/90 ordered-failure comparisons.
The immutable harness checks runtime Deprecated annotations on both entries;
complete compiler metadata comparison also checks the Deprecated attributes.
The same full class argument and unchecked ancestor counterexamples remain in
place. Native controls reject mismatched markers, suppression and bridge
parameter annotations. Coverage remains `partial` only for that original bridge.

`test_mobile_android_class_formal_implementation_bridges.py` retains original
class-owned generic implementation Signatures and receiver-first static generic
helper calls. Source and immutable interface/helper-JAR profiles keep four
classes/fourteen methods and two classes/seven methods, respectively, apart from
reported private source helpers. Each immutable JVM oracle compares 112 values,
535 identities, 416 states and 80 ordered failures through eight routes and an
independent subclass. Original virtual callbacks, recursive Comparable bounds,
mutable aliases, comparison effects, nulls and exact exception identities agree.
Complete class, field, method and exception metadata stays unchanged except for
two disclosed original ACC_BRIDGE flags. Same-Code substitution of an unrelated
class formal and an independent javac name-clash counterexample are rejected.
Coverage remains `partial` for those two original flags.

`test_mobile_android_integer_static_comparisons.py` retains a plain typed integer
implementation marked ACC_BRIDGE and both original erased comparison entries.
Source and unchanged helper-JAR modes compare 80 values, 144 identities, 237
states and 37 failures across eight direct, reflected, MethodHandle and original
caller routes. Integer overflow, repeated alias arguments, virtual callback order,
null/wrong inputs and exact callback exceptions retain their original behavior.
All runtime inventory and generic metadata remains unchanged; the report covers
the typed bridge flag and two verified compiler bridges. Same-Code synthetic
flag drift, independently authored reversed helper arguments and an extra effect
must fail. `--dependency-helper` keeps the input interface contract and executes
against the original helper/trace JAR. Coverage remains `partial`.
The `--reference-result` profile also retains typed and covariant reference
entries with one unchanged long argument. Its additional immutable oracle
compares 152 values, 296 identities, 456 states and 16 failures across eight
routes, including high/low word boundaries, alias/null results and exact callback
failures. Source and `--dependency-parent` modes keep six classes/twenty-two
methods and five classes/twenty-one methods. Both original typed bridge flags
and four verified compiler bridges are reported; all other metadata is unchanged.
A changed wide argument and synthetic typed flag are additional counterexamples.

The `--inherited-predicates` profile adds two boolean entries declared by a
plain ancestor two edges above the closed receiver interface. Its separate
immutable oracle compares 80 values, 96 identities, 288 states and 32 failures,
including inherited reflection lookup, original helper callbacks, exact failure
identity and null receivers. Source and `--dependency-parent` modes retain seven
classes/twenty methods and five classes/eighteen methods. Three typed flags and
two original compiler bridges are reported; all other metadata stays unchanged.
Synthetic predicate flags and independently authored wrong receivers must fail.

`--omit-interface-bridge` removes only the owned fixture interface's original
compiler bridge before DEX creation, keeping all other Code and metadata intact.
The original five classes/twelve methods (or three classes/nine methods with
`--dependency-helper`) remain in the recovery inventory. One additional compiler
interface bridge is reported separately, with three projected original methods.
The ordinary comparison oracle still matches; an additional immutable oracle
checks 10 values, 22 identities, 34 states and 4 failures through inherited
reflection and MethodHandles. It measures the disclosed added default and
bridge declaration. Wrong complete interface arguments and a missing concrete
class bridge are rejected; coverage remains `partial`.

`--copied-receiver` authors explicit receiver copies and casts before the original
helpers. The integer, covariant reference and inherited predicate profiles use
their existing independent oracles and unchanged inventory/metadata checks.
The combined reference/predicate profile keeps eight classes/twenty-nine methods,
or five classes/twenty-six methods with unchanged parent declarations in a JAR,
and reports eight projected methods. The integer-only helper-JAR mode remains
three classes/ten methods. Complete cast receiver provenance, wide arguments,
original callbacks and ordered exceptions must agree; changed receivers and
extra effects remain rejected.

`test_mobile_android_comparable_scalar_predicates.py` checks the original
Comparable-to-Character check, char unboxing and scalar predicate dispatch under
two recursive generic interface bounds. Source and immutable ancestor-JAR modes
compare direct, raw/typed interface, reflective, MethodHandle and original caller
routes, including mutable parent effects, getter identity and ordered failures.
The report discloses two raw source interface edges and the retained predicate's
bridge/synthetic flag difference. Runtime descriptors and the unrelated generic
superclass remain unchanged. Same-Code wrong arguments and missing synthetic
flags fail; independent javac rejects the unprojected source name clash.
The `--numeric` profile adds Double predicates with one or two wide arguments
and an inherited interface scope. Its immutable Number subclass records the
original virtual conversion order and throws exact preallocated failures. The
matrix also covers NaN, infinities, signed zero, Float inputs, repeated references
and exceptions before and after target effects. Missing high words, swapped
arguments, changed unbox declarations and divergent ancestor formals are rejected.

`test_mobile_android_comparable_carrier_predicates.py` extends that boundary to
original int/long carrier conversions and getter factories. The default long and
`--narrow` int profiles each support source and immutable ancestor/carrier-JAR
modes. Eight immutable JVM call routes compare 96 values, 346 identities,
624 state observations and 88 ordered failures. Two-argument calls retain each
conversion's order, including repeated inputs, wrong second arguments, nulls,
conversion failures and target failures after effects. Getter checks preserve
factory frequency, fresh-object identity, original scalar words and exceptions
before or after factory entry. Runtime inventories and unrelated generic metadata
remain unchanged. Two raw interface edges and three retained bridge flag
differences are disclosed; the source carrier's ordinary comparison bridge is
regenerated. Same-Code wrong interface arguments and missing synthetic flags,
plus an independent javac name clash, remain rejected.

The `--inlined-carrier` combination retains an original checked comparison body
whose carrier interface already has a certified source projection. Scalar
conversion validation reads the saved original class contract, while the original
body and the existing comparison projection remain independently validated.
Source and immutable ancestor-JAR modes keep the carrier in the input. Each int
or long run compares 105 values, 347 identities, 630 states and 94 failures,
including direct, reflective and MethodHandle comparison of the carrier itself.
Wrong original casts, missing bridge evidence, open class scopes and incompatible
recursive variance still fail. Reports disclose both existing projections.

`test_mobile_android_raw_generic_ancestor_returns.py` checks raw generic
interface return contracts through classes without class Signatures. The
admission proof checks the complete plain superclass chain, every matching
original override and all raw interface requirements before selecting raw member
semantics. Source and immutable ancestor-JAR profiles compare 18 values, 153
identities, 117 states and 27 failures over nine original, inherited, reflective
and MethodHandle routes. Mutable iterator state, aliases, parent effects and
exception identity match. All original generic metadata and runtime entries
remain unchanged; no source projection is added and coverage is `recovered`.
Same-Code unproved method formals fail NeverD and independent javac. Native
controls also reject incompatible results, reduced visibility, static overrides,
missing/cyclic ancestry and a hidden final ancestor override.
The `--covariant-target` variant checks a wider original bridge and its narrower
implementation against separate raw interface requirements, with the same runtime
oracle and unchanged original metadata. Its one original compiler bridge is
reported as source-projected; a same-Code missing-synthetic mutation is rejected.

`test_mobile_android_protected_scalar_array_targets.py` verifies public erased
Object bridges to protected primitive-array targets with integer results. All
eight primitive arrays keep their original methods, superclass bindings and
bodies. Source and unchanged parent-JAR profiles compare 192 values, 1,024
identities, 1,152 states and 256 failures over direct typed wrappers, original
callers, parent callbacks, reflection and MethodHandles. Casts before target
effects, null failures, mutable aliases and exact exceptions remain unchanged.
`public-erased-protected-parameter-target` discloses only the typed target's
protected-to-public source access; the original public erased descriptor and
flags survive javac regeneration. Original metadata otherwise stays unchanged
apart from reported private helpers. Same-Code wrong array bindings and missing
synthetic flags fail; independent javac rejects the wrong array override.
Coverage remains `partial` for the two reported entries per array class.

`test_mobile_android_protected_void_consumers.py` checks an independently scoped
open generic owner with a public erased Object bridge to a protected closed
reference consumer returning void. Source descendants inherit the original
implementation. The common full substitution and register-origin proof keep
checked casts before target effects; the typed source access change and its
consequence for recompiling protected overrides are disclosed. An immutable
external protected override checks virtual dispatch through parent callbacks,
typed wrappers, reflection, MethodHandles and a separately compiled raw caller.
Nullable mutable inputs, ordered state changes and exception identities are
compared. `--dependency-parent` keeps the original parent in an unchanged JAR.
Wrong original flags or superclass arguments fail, and independent javac
rejects the incompatible override. Coverage remains `partial`.
The source profile retains four classes and nine original methods; the SDK
profile retains three input classes and six original methods. Both compare
216 values, 510 identities, 1,044 state observations and 186 ordered failures.

`test_mobile_android_empty_original_bridge_throws.py` checks an original
parameter-specializing bridge and typed implementation with empty Exceptions attributes against
closed checked ancestor declarations. Source and immutable ancestor-JAR profiles
compare 48 values, 576 identities, 324 states and 72 failures through twelve
direct, inherited, original-caller, reflective and MethodHandle routes. The
original checked super call, late result cast, mutable aliases, nulls and exact
exception identities remain intact. The original typed declaration stays empty;
`source_bridge_declared_throws` and `bridge_exception_source` disclose javac's
regenerated ordered bridge Exceptions. Original runtime slots and other metadata
remain unchanged apart from reported private helpers. Same-Code full-parent and
synthetic-flag mutations fail, and independent javac rejects the incompatible
complete parent binding. Native controls reject missing closed receipts, open throws, changed
parameter forwarding and conflicting inherited exception order. Coverage remains
`partial` for the original compiler bridge.

`test_mobile_android_fixed_exception_covariance.py` retains a generic source
parent returning its complete `Parent<T>` type, an intervening plain superclass
and a concrete child returning itself. All original declarations and bridges
keep ordered fixed checked Exceptions. Source and immutable far-superclass-JAR
profiles retain six classes/twelve methods and five classes/ten methods, apart
from reported private helpers, and regenerate three original bridges. Each
immutable JVM profile compares 54 values, 1,728 identities, 180 state observations
and 126 ordered failures through direct, near/far superclass and typed/erased/
reflected routes, including an independent override, mutable field aliases,
exception identities and casts after effects. Complete generic parent results,
metadata and runtime entries recompile unchanged. Same-Code generic parent result
and non-Throwable ancestor mutations fail NeverD and independent javac. Native
controls cover equivalent fixed sets, empty narrowing, separate parameterized
member profiles, source/SDK roots and multiple ancestor bridges. Wider or mixed
checked throws, generic/unknown throws, altered invariant parameters or full
results, bridge drift and missing closed receipts fail. Coverage is `partial`
only for original compiler bridges; no generic metadata projection is added.

`test_mobile_android_erased_terminal_consumers.py` retains a final owner's
original erased `(Object) -> boolean` interface bridge when no typed reference
entry exists and its exact body only allocates an UnsupportedOperationException,
resolves the original message, invokes its constructor and throws. The common
terminal-body proof permits only local registers, with no parameter read,
handler, branch, extra operation or declared exception. The complete unary
Object-bounded interface substitution is retained separately; the inheritance
planner rechecks this receipt and every other original full member contract.
No typed method, input cast, primitive read or replacement exception is added.

`erased-terminal-consumer-raw-interface` discloses the source raw interface.
Raw source members must also erase parameter arguments and method-owned
formals/results to avoid javac name clashes. These emission-only signatures
are recorded individually as `erased-terminal-raw-member`; original full method
plans remain unchanged for validation. Closed parameterized results remain
complete. `sourceMethodSignature` supplies the same checked source view to
method declarations, return expressions and metadata reporting. Other roots
are restricted to known empty nongeneric markers; other inherited slots still
must prove their full original contracts. Source bounds are rechecked normally.

The authored class-file/D8/JVM source and immutable interface/ancestor-JAR
profiles retain five classes/16 methods and three classes/10 methods apart from
reported helpers. One original bridge and four source member erasures are
reported. The immutable harness compares 174 values, 282 identities, 426 states
and 102 failures through direct, interface, reflection and MethodHandle routes.
It checks compatible, wrong-type and null mutation arguments, unchanged exception
allocation/message, dormant target failures, arrays/aliases, effects and
exception identity/order in normal members, and closed iterator result metadata.
Class/member metadata changes are limited to the raw edge, four method Signatures
and bridge flags. Same-Code full bulk-parameter drift fails NeverD and independent
javac; a missing original synthetic flag also fails. Native controls reject
incoming register reads, body drift, altered scoped/parameterized contracts,
nonfinal owners and missing inherited implementations. Coverage remains `partial`.

`test_mobile_android_guarded_scalar_parameters.py` retains an original final
Object-parameter boolean bridge with an exact type guard, false return, cast,
zero-argument unbox and same-owner scalar call. The planner requires the known
checked type, unchanged incoming argument/receiver, precise false branch,
scalar/wide capture and adjacent wide argument words. Both original callees
are public instance, Signature-free declarations with no declared exceptions;
their complete bodies remain ordinary recovery obligations. The literal
class interface arguments and every other member contract remain unchanged.
`guarded-scalar-parameter-bridge` records the original target, unbox and inherited
member while reporting bridge and any synthetic flag differences.

The authored class-file/D8/JVM source and immutable interface-JAR profiles
retain five classes/14 methods and four classes/11 methods apart from reported
helpers. Two bridges exercise long and double unboxing. The immutable harness
compares 699 values, 1,239 identities, 1,980 states and 48 failures through
direct, typed/raw interface, reflection and MethodHandle routes. It checks
wrong-type/null false returns with no callee effects, wide/raw floating bits,
shared array aliases, mutable target fields, and unbox-before-target exception
identity/order. Complete metadata remains unchanged except bridge flags and
reported helpers. Same-Code method-formal drift fails NeverD and javac; a
missing original final flag also fails. Native profiles cover all eight scalar
kinds and reject changed type/branch/false value, incoming register, wide pair,
call kind, access, metadata or extra unimplemented slots. Coverage is `partial`.

`test_mobile_android_named_scalar_getters.py` retains an original public
bridge/synthetic method that implements an unchanged plain scalar interface
slot by calling a differently named, public instance getter on the same final
owner. The exact three-operation body must preserve the incoming receiver,
zero-argument virtual call and scalar or wide result capture/return. Both
runtime declarations remain Signature-free; the original class interface
arguments and independent method-owned contracts stay unchanged. The target's
full body is recovered ordinarily, including effects and ordered exceptions.
`named-scalar-getter-bridge` records the original target, inherited member and
bridge/synthetic flag differences without renaming or adding a runtime entry.

The authored class-file/D8/JVM source and immutable interface-JAR profiles
retain three classes/16 methods and two classes/11 methods, apart from reported
private helpers. Four bridges cover int, long, float and double results. The
immutable harness compares 401 values, 1,991 identities, 1,272 states and 48
failures through direct, typed/raw interface, reflected and MethodHandle calls,
including floating raw bits, mutable field/array aliases and exception identity.
All generic and class/member metadata remain unchanged except reported bridge
flags and helpers. Same-Code full method-formal drift fails NeverD and javac;
a missing original synthetic flag also fails. Native controls reject malformed
frames, changed receivers/results, metadata, access, extra operations and
unimplemented interface slots. Coverage remains `partial`.

`test_mobile_android_boxed_generic_interface_results.py` retains an original
Object-returning generic interface bridge with an exact five-operation body:
call the unchanged zero-argument primitive target, capture its scalar or wide
result, call the original static boxing factory, capture and return that object.
The common validator checks all registers, wide pairs, exact declarations,
closed plain result binding and the full inherited contract. Target and factory
bodies remain ordinary recovered methods, including their effects, exceptions,
allocation, caching and nullable results. No pure-field assumption, replacement
boxing implementation or typed runtime getter is introduced.

The source interface argument becomes explicit Object under
`boxed-interface-result-argument`; the original complete edge remains the
inheritance planner's contract. Other slots still receive ordinary full member
proofs. A class-formal default stays in its unchanged declaring interface only
when the input does not override that slot; conflicting abstract consumers or
extra parameterized roots are rejected. Empty nongeneric marker roots remain.
`boxed-generic-interface-result-bridge` reports the exact primitive target,
factory, original interface member and bridge/synthetic flag differences.
All source bounds are checked after the projection.

The `--abstract-owner` profile retains an abstract scalar getter and two original
result-only return entries. A public final typed entry must independently prove
the same five-op virtual getter/factory path; only its Java source name changes
under the reported `incompatible-inherited-return-slot` receipt. The original
Object bridge keeps its name and body. Subclass getter effects and ordered
exceptions remain original virtual calls. The authored `Child` supplies the
scalar body, and the immutable harness compares 246 values, 804 identities,
636 states and 42 failures across eight direct/interface/reflected/MethodHandle
routes and inherited defaults. Source and interface-JAR profiles retain five
classes/16 methods and four classes/11 methods, apart from reported helpers.
They check complete metadata and inventory; same-Code argument drift, missing
synthetic flags and missing typed final flags are rejected. An independently
compiled conflicting complete result also fails javac.

Adding `--deferred-plain-slot` to `--abstract-owner` moves the original `hasNext`
implementation to its concrete child. The abstract owner declares no replacement
method. Its Signature-free inherited slot keeps exactly the original full
parameter/result types even while the unrelated boxed result argument projects
to Object. Same-slot shadows, method formals and invalid concrete providers
remain fail-loud. Source and interface-JAR profiles use the same immutable
246/804/636/42 semantic matrix and complete metadata/inventory checks.

The independently authored class-file/D8/JVM fixture changes only its interface
Signature and bridge flags, preserving all Code and descriptors. Source and
immutable interface-JAR profiles retain four classes/13 methods and three
classes/eight methods apart from reported helpers. An immutable harness compares
186 values, 610 identities, 484 states and 34 failures across direct, typed/raw
interface, reflected and MethodHandle calls. It checks wide extremes, new/cached/
null factory results, mutable aliases, target-before-factory exceptions, iterator
state, default callback dispatch and callback/null/remove failures. Complete
class/method metadata changes are limited to the reported Object interface
argument and flags. Same-Code complete result-argument and synthetic-flag
counterexamples are rejected. Coverage remains `partial`.

`test_mobile_android_final_void_return_slots.py` retains a final void JVM
entry beside a same-parameter inherited boolean entry. The shared source-name
planner compares complete generic parameters across the nearest actual class
provider path before recording `incompatible-inherited-return-slot`. Both
original bodies and caller dispatch remain, with the rebuilt name/reflection
difference disclosed. Source and immutable ancestor-JAR profiles keep five
classes/11 methods and two classes/six methods apart from reported helpers.
Thirteen void/scalar routes compare 56 values, 130 identities, 403 states and
26 failures, including repeated mutation, aliases, original callers, defaults,
reflection, MethodHandles, null receivers and exact exceptions. Complete
metadata changes only at the recorded void source name. Same-Code wrong
ancestor binding and missing final flags fail; independently authored
incompatible return slots fail javac. Coverage remains `partial`.

`test_mobile_android_nested_object_lower_returns.py` verifies closed nested
Object arguments against inherited Object lower wildcards. The common proof
records `closed-object-lower-bound` while retaining every original Signature,
body and runtime slot. Source and unchanged interface-JAR profiles compare
18 values, 153 identities, 117 states and 27 failures across nine routes,
including original callers, defaults, reflection and MethodHandles. Mutable
iterator identity, removal, alias updates, nulls and exception order remain.
Complete metadata stays unchanged. Independent javac accepts the original
closed return without unchecked conversions and preserves its Signature;
wrong complete arguments and bounds fail. Coverage is `recovered`.

`test_mobile_android_boxed_superclass_results.py` checks an original indexed
scalar/factory bridge through a generic superclass, a further class ancestor
and an interface. The common validator compares every original parameter and
result before reporting `boxed-superclass-result-argument`; all runtime slots,
original callers and bodies remain unchanged. Source and immutable ancestor-JAR
profiles compare 330 values, 1036 identities, 820 states and 42 failures across
nine call routes and inherited interface callbacks. Wide extrema, mutable arrays,
repeated calls, new/cached/null factory results, aliases, ordering and exact
exceptions remain intact. Complete metadata permits only the reported Object
superclass argument and bridge flags. Same-Code wrong result arguments and lost
synthetic flags fail; an independently authored incompatible result fails javac.
Coverage remains `partial`.

`test_mobile_android_inherited_wildcard_parameters.py` checks a final generic
child whose complete parameter matches an actual superclass argument bounded
by an upper-wildcard generic interface. Its inherited same-slot bridge takes
unbounded wildcard arguments. The shared planner records only the child's
source parameter erasure under `inherited-wildcard-parameter-erasure`, retaining
original runtime slots, bodies and complete class bindings. Source and immutable
ancestor-JAR profiles retain six classes/16 methods and three classes/eight
methods, apart from reported helpers. Eight routes compare 70 values, 348
identities, 740 states and 38 failures across original callers, reflection,
MethodHandles and the inherited guard. Repeated mutations, getter ordering,
aliases, nulls and exact exceptions remain unchanged. Complete metadata checks
allow only the reported child method Signature and source parent bridge flags.
Same-Code wrong parameter scope and superclass binding fail; an independently
authored incompatible wildcard override fails javac. Coverage remains `partial`.

`test_mobile_android_raw_method_formal_members.py` checks original raw
interfaces on a final Object-based owner without a class Signature. A complete
alpha-equivalent method-owned contract becomes its erased source subsignature
under `raw-interface-method-formal-erasure`; every original bound, parameter,
result and interface ancestor remains part of the proof. The original raw edge,
runtime entry and body remain unchanged, and all source users share that receipt.
The authored source and immutable interface-JAR profiles retain three classes/
seven methods and one class/four methods, apart from reported helpers. Nine
call routes compare 45 values, 432 identities, 432 states and 18 failures,
including a method-generic default, original caller, mutable aliases, arrays,
nulls and ordered exceptions. Complete metadata changes only at the disclosed
method Signature. Same-Code result/tag scope drift is rejected, as is an
independently authored incompatible result checked by javac. Coverage is
`partial` with no unrecovered methods.

`test_mobile_android_raw_constant_parameters.py` verifies a raw generic
interface implementation whose closed parameter Signatures cannot directly be
written as a Java override. Only a final independent class with an exact
constant field-read body qualifies: the original method never uses its inputs,
reads the known public static final reference, and returns it. The report names
`raw-interface-constant-parameter-erasure` and preserves both Signatures and
the original interface contract. No interface argument is invented. Source and
immutable declaration-JAR profiles compare ordinary, raw, typed, reflection,
MethodHandle and independent caller routes. Null and throwing argument objects
are never accessed; singleton identity, mutable result aliases, initialization
ordering, first initialization failures and subsequent erroneous-class failures
remain checked. Complete metadata changes only at the reported method Signature;
coverage is `partial` with no unrecovered methods. Same-Code method-formal and
unknown-scope counterexamples fail recovery, and independent javac rejects the
unprojected typed override of the raw interface.
The source profile retains six classes and eight methods, and the SDK profile
one class and two methods. The oracle compares 144 values, 296 identities,
369 states and 144 failures.

`test_mobile_android_checked_inlined_interfaces.py` retains an erased interface
bridge whose implementation has been fully inlined, with no typed runtime
method to regenerate. The common validator requires a final, independently
scoped Object-based owner, one direct unary Object-bounded interface, a
complete plain nominal argument, one abstract parameter-formal/primitive-result
contract and the original public bridge/synthetic flags. Only unchanged
incoming object copies and nops may precede the exact argument cast. The entire
original body then passes ordinary instruction and source recovery; no
unboxing, comparison or virtual target is inferred from a name. A remaining
same-owner virtual scalar target with parameters must retain its original
forwarding proof. The explicit `Object` interface argument is disclosed as
`checked-inlined-interface`, while `checked-inlined-interface-bridge` retains
the original Object parameter, primitive result, full body and runtime entry.

The authored class-file fixture changes only the class Signature and original
bridge/synthetic flags before D8; all Code and descriptors remain unchanged.
Source and immutable interface-JAR profiles retain three classes/five methods
and two classes/four methods apart from reported private helpers. An immutable
harness compares 156 values, 774 identities, 570 states and 36 failures across
direct, raw/typed interface, reflection and MethodHandle calls. Wide edge
values, self/argument aliases, mutable state, argument-before-receiver unboxing
and cast/ null/exception order remain matched. All metadata changes are checked
against the report. The complete original interface edge remains the
inheritance planner's contract; source declares `Api<Object>` so its Object
slot also satisfies a dependent `Api<? super Core>` bound. All source bounds
are rechecked normally. Invariant and upper-bounded constraints that need the
original argument still fail explicitly. Same-Code complete argument drift and
missing original synthetic flags are rejected. Native controls cover source/SDK
roots, direct/copied casts, self and separate arguments, scalar/wide results
and complete contract failures. Coverage remains `partial` for the interface
argument and original bridge flags.

The `--recursive-bound` profile adds an original `Holder<T extends Api<? super
T>>` and a consumer whose field and return retain `Holder<Core>`. Source and
immutable interface-JAR profiles retain five classes/twelve methods and four
classes/eleven methods apart from reported helpers. An independent immutable
harness compares 286 values, 1,899 identities, 1,045 states and 66 ordered
failures over eleven routes, including typed/raw holder calls and complete
consumer return casts. Holder bounds, class-owned member scopes and consumer
metadata remain exact; only the input bridge's interface argument and flags
differ. Independent javac rejects the raw-interface version of the bound and
accepts the explicit Object argument. Native controls retain the lower bound
while rejecting invariant and upper bounds after source projection, preserving
the ordinary source check.

`test_mobile_android_package_generic_covariance.py` retains a package-visible
generic parent getter and its public covariant override. The original wildcard
leaf result implements the exact complete result-interface argument, and its
public return bridge retains the original runtime slot. Source and immutable
parent-JAR profiles keep five classes/seven methods and four classes/five
methods, full original class/method metadata and all runtime entries apart from
reported private helpers. An immutable JVM harness compares 56 values, 322
identities, 168 states and 28 failures across direct, typed/raw parent,
reflection and MethodHandle calls with an independent override. Mutable aliases,
null receivers and original ancestor exceptions stay matched. Original D8 bridge
opcodes are checked; a same-Code result-interface argument change fails NeverD
and independent javac. Native controls retain complete parameters, public/package
bridge roles and visibility over the entire ancestor path, including default
package access bridges. Coverage is `partial` for one original compiler bridge,
with no generic metadata or class-source projection.

`test_mobile_android_public_superclass_access_bridges.py` retains six original
public access bridges over a nonpublic generic superclass and public plain
ancestor. Original void, boolean, integer, long, double and reference bodies
keep exact superclass dispatch and complete class-owned getter scopes. The
immutable harness compares 368 values, 548 identities, 1928 state observations
and 120 failures over direct, public-ancestor and reflected calls, including an
independent public override. Mutable aliases, NaN/zero bits, original ancestor
exception identities and null receivers stay matched. Source and immutable
plain-ancestor-JAR profiles keep three classes/24 methods and two classes/16
methods, every runtime entry apart from reported helpers and all metadata apart
from reported bridge/synthetic flags. Original D8 access-bridge opcode witnesses
are checked; a same-Code final ancestor mutation fails NeverD and independent
javac. Coverage is `partial` for six original bridges, with no generic metadata
or class-source projection.

`test_mobile_android_scoped_raw_class_parameters.py` retains a Signature-free
class target overriding a complete scoped ancestor parameter through its
original checked bridge. The target's exact superclass adapter keeps the
original receiver, parameter order, scalar result and super dispatch. Source
and immutable ancestor-JAR profiles retain six classes/nine methods and four
classes/six methods, complete original class/method metadata and all original
runtime entries apart from reported private helpers. An immutable JVM harness
compares 108 values, 558 identities, 360 states and 72 failures across direct,
typed/raw ancestor and reflected calls, including an independent override.
Mutable aliases, cast failures before effects, null receivers and target
exceptions stay matched. Original D8 bridge/super opcode witnesses are checked;
a same-Code class-bound change fails NeverD and independent javac. Coverage is
`partial` only for one original compiler bridge, with no generic metadata or
class-source projection.

`test_mobile_android_scoped_raw_interface_parameters.py` applies the same proof
to a complete interface diamond. The original class formal keeps its nominal
bound, the target keeps its Signature-free nominal parameter, and javac
regenerates the exact original erased bridge. Source and immutable interface-JAR
profiles retain eight classes/13 methods and four classes/11 methods. Seven call
routes, separate source and frozen callers, and an external subclass compare
73 values, 156 identities, 161 states and 56 failures. Every original class and
method metadata item and Exceptions attribute stays unchanged. Same-Code class
bounds, diamond bindings and missing bridge flags are rejected; an independent
javac counterexample checks the class-bound rule. Native checks also compose
interfaces through generic superclasses and reverse class, member and edge
traversal order.

`test_mobile_android_scoped_plain_ancestors.py` retains an abstract generic
source parent over a nongeneric ancestor with complete method-owned interface
contracts and fixed checked Exceptions. Its generic child preserves its own
class formal and getter while inheriting the original ancestor methods.
Source and immutable ancestor/interface-JAR profiles retain six classes/eleven
methods and four classes/six methods, complete original metadata and runtime
inventory apart from reported private helpers, with zero bridges or metadata
projections. The immutable JVM harness compares 58 values, 546 identities,
104 state observations and 56 failures through direct/interface/reflected
inherited calls, parent getters and an independent override. Mutable aliases,
Class casts after effects and original exception identities stay matched.
A same-Code ancestor method-bound change fails NeverD and independent javac;
Native controls also cover full parameterized contracts, alpha-equivalent
method scopes, source/SDK ancestors, final/static/private roles, erased or
projected signatures, missing constructors and unknown ancestry. Coverage is
`recovered`.

`test_mobile_android_primitive_bridge_copies.py` retains four original package
bridges for a class-owned bounded parameter alongside int, float, long and
double arguments and results. D8 disassembly witnesses confirm all four
bridges actually exercise scalar/wide moves and a ranged virtual invocation.
Source and immutable parent-JAR profiles keep two classes/fourteen methods and
one class/nine methods, complete generic/access metadata and original runtime
entries apart from reported private helpers and regenerated compiler bridges.
The immutable JVM harness compares 720 values, 3,648 identities, 912 state
observations and 192 failures through direct, typed/raw parent and reflected
routes, including an independent override. Integer extremes, NaN payloads,
negative zero, infinities, mutable aliases, target exception identities and
checks before target effects remain matched. A same-Code method-owned formal
mutation fails NeverD and independent javac. Native profiles also cover
source/SDK class and interface roots, public/package roles, all move encodings,
overlapping wide copies and wrong/high-word/uninitialized/changed origins.
Coverage is `partial` only for the four original compiler bridges; no generic
metadata projection is added.

`test_mobile_android_fixed_exception_defaults.py` retains a generic source or
immutable interface-JAR default with fixed checked Exceptions, a complete
invariant carrier parameter and a concrete override with its original erased
return bridge. Both exception lists and declaration order remain original.
Each profile compares 33 values, 966 identities, 100 state observations and
73 ordered failures, including direct/interface/reflected override dispatch,
independent subclass dispatch and execution of the untouched default itself.
Mutable aliases, casts after effects, exception identities, complete metadata
and original runtime entries recompile unchanged apart from reported helpers
and the regenerated original bridge. Source and dependency profiles retain
five classes/seven methods and four classes/six methods. Same-Code full parameter
and non-Throwable ancestor mutations fail NeverD and independent javac. Native
controls cover same-descriptor defaults, empty narrowing, source/SDK roots,
order independence, missing closed receipts and altered original bridge metadata
or instructions. Coverage is `partial` only for the original compiler bridge;
no generic metadata projection is added.

`test_mobile_android_unchecked_covariant_parameters.py` keeps an original
covariant forwarding bridge whose typed target has a complete invariant carrier
parameter and its own ordered unchecked Exceptions declaration. A nongeneric
interface returns itself; the implementation returns its concrete source class.
Source and immutable interface-JAR profiles retain five classes/seven methods
and four classes/six methods, apart from reported private helpers, and regenerate
only the original erased bridge. Each immutable JVM profile compares 30 values,
960 identities, 100 state observations and 70 ordered failures through direct,
interface and typed/erased/reflected routes, including an independent override,
mutable carrier aliases, original exception identities and field casts after
effects. Complete metadata and original runtime entries recompile unchanged.
Same-Code full invariant argument and unchecked-ancestor mutations fail NeverD
and independent javac. Native controls also cover SDK Number/Byte covariance,
missing closed/unchecked receipts, checked/mixed/generic/unknown throws, missing
Signatures and altered bridge flags, receiver/argument origins or invocation.
Coverage is `partial` for the original bridge; no generic metadata projection is
added.

`test_mobile_android_scoped_unchecked_exception_narrowing.py` keeps a generic
subinterface's original unchecked Exceptions declarations while its parent
interface declares an independent checked exception. Class-owned `read` and
method-owned `unwrap` contracts remain complete in the intermediate declaration
and implementation. Source and immutable root-JAR profiles retain six
classes/ten methods and five classes/eight methods, apart from reported private
helpers, with zero bridges or metadata projections. Each immutable JVM profile
compares 72 values, 804 identities, 156 state observations and 84 ordered failures
through direct, parent/subinterface and reflected routes, including an independent
override, mutable aliases and casts after effects. Original ordered Exceptions,
full generic metadata and runtime inventory recompile unchanged. Same-Code
method-bound and unchecked-ancestor mutations fail NeverD and independent javac.
Native controls also require both closed exception receipts and the unchecked
receipt, and reject generic throws, wider checked/mixed hierarchies, full scope
drift, unknown exceptions and changed member roles. Coverage is `recovered`.

The `--exception-subsets` profiles of
`test_mobile_android_plain_fixed_exceptions.py` retain narrower nonempty original
Exceptions declarations beneath an interface's wider fixed list. The target
keeps the same ordered checked exceptions while the interface additionally
declares an unchecked exception; its method-owned generic member retains the
complete original scope. Source and immutable interface-JAR profiles preserve
five classes/eight methods and four classes/six methods, zero bridges or metadata
projections, and compare 48 values, 536 identities, 104 state observations and
56 ordered failures. Full metadata and runtime inventory recompile unchanged
apart from reported private helpers. An Exceptions-only change removing the
required checked exception, with Code and Signatures unchanged, fails NeverD
and independent javac; the original method-bound counterexample also fails.
Native profiles cover source/SDK roots, abstract subinterface declarations,
reordered subsets, individual checked entries, complete method scopes and
missing closed receipts. Wider checked lists and inferred subtype narrowing
remain outside this exact subset proof.

`test_mobile_android_plain_fixed_exceptions.py` checks a nongeneric interface
with fixed checked exception declarations and a generic consumer. Its method
`<T> T unwrap(Class<T>)` keeps the complete method-owned scope alongside its
fixed Exceptions attribute. Source and immutable root-JAR profiles retain four
classes/seven methods and three classes/five methods respectively, apart from
reported private helpers, with zero bridges or metadata projections. Each
immutable JVM profile compares 48 values, 536 identities, 104 state observations
and 56 ordered failures through direct, interface and reflected calls, including
an independent override, mutable aliases, exception identity and Class casts
or null checks after original effects. Same-Code method-bound drift fails NeverD
and independent javac. Native controls include empty exception narrowing and
reject unknown or wider throws, duplicate entries, generic throws, altered
bounds and arguments, wrong access/static/abstract roles and missing common
exception receipts. Coverage is `recovered`.

`test_mobile_android_recursive_unchecked_callbacks.py` checks a concrete callback
with original RuntimeException and Error declarations beneath two recursively
bounded source class scopes. The abstract parent retains its plain interface
obligation without an invented callback, while its forwarding method and the
interface's original default dispatch to the concrete child. Source and immutable
root-JAR profiles retain seven classes/ten methods and six classes/eight methods
respectively, apart from reported private helpers, with zero bridges or metadata
projections. Each immutable JVM profile compares 36 values, 360 identities,
90 state observations and 54 failures through direct, parent, interface, default
and reflected calls, including an independent override, mutable byte-array
aliases, nulls and original exception identities after effects. Same-Code
unchecked-to-checked hierarchy drift fails NeverD and independent javac. Native
controls also reject mixed or unknown throws, missing shared exception receipts,
method-formal drift, wrong access/static/abstract roles and missing providers.
Coverage is `recovered`.

A class-owned result formal can likewise retain its original descriptor when
its sole bound is one known plain class or interface and the required result is
that exact plain bound. The common proof compares complete parameters and the
resolved result scope; equal erasures alone do not suffice. Unknown,
parameterized or intersection bounds, method-formal shadows and required type
variables remain outside this result receipt. A source parent's plain interface
root with fixed parameterized class ancestry enters the shared complete generic
member queue, including its existing SDK Object and original default bridge
proofs. Method-only plain roots retain their previous complete-method path.
Every root and nearest declaration remains checked independently.

The bounded-formal-object-contracts source and `--dependency-root` JVM profiles
combine a bounded getter and fixed callback contract, inherited Object equality,
hashing and string methods, a fixed interface's original checked comparison
bridge, and a fully closed `Class<? extends Bound>` result from a plain ancestor
interface. They check typed, raw, interface and reflective dispatch, mutable result
identity, nulls, callback effects, casts before default effects and ordered
exceptions. Class-token calls preserve returned identity, full wildcard bounds,
original declaring owners and the absence of unrelated getter effects. The
dependency profile moves the generic root and the plain class-token declaration
into an unchanged JAR; the other intermediate interfaces remain source declarations. Complete original
class, field and method metadata is compared, and the closed child acquires no
getter, comparison or Object implementation. Same-Code method-scope, callback
argument, generic-root and full class-token result mutations, plus private, static and abstract Object
shadows, are rejected with independent javac counterexamples. These profiles run
with JDK 21. The original interface bridge is regenerated and reported as
`partial`; no generic Signature or class binding is projected.
The additional `--dependency-root --default-type` profile places the class-token
implementation in the source subinterface's default method. The complete closed
result contract still matches the external declaration, and class consumers
inherit the original source owner without gaining a method. Reflection and the
same behavior checks retain this owner; the same-Code result counterexample
changes that default's Signature while the dependency declaration remains
unchanged.

A parameterized abstract SDK class may defer an interface obligation through
its original complete class hierarchy to a nearer SDK class or source subclass.
The shared member queue still proves the implementation's complete scoped types,
visibility and concrete body; an admission check cannot require every abstract
ancestor to implement the slot itself. The sdk-abstract-interface-roots fixture
retains a generic PriorityQueue subclass and its original own offer override.
Direct, Queue interface and reflective calls preserve element identity, priority
ordering, inherited iterator removal, mutable effects and null failures. Original
class/member metadata and the entire inventory remain unchanged, with zero source
projections. A same-Code parameter mutation changes the original class formal to
another equally erased formal and is rejected, with an independent javac
counterexample.

A zero-argument reference default declared by a generic source interface may
inherit an SDK slot when its own class formals have exact Object bounds and the
common complete ancestor/result proof matches. The scoped-source-defaults source
and `--dependency-interfaces` profiles retain the original class-owned result
variable and default declaring owner. Direct, source interface, generic and raw
SDK interface, and reflective calls preserve returned-self identity, callback
effects and null receiver behavior. Complete class and method metadata and the
entire original inventory remain unchanged; consumers acquire no copied getter
and coverage is `recovered`. A same-Code result mutation substitutes a fixed
String argument for the original class formal and is rejected, as is an
independent javac counterexample.

Inherited method formals may also retain one complete known class bound per
formal, with no additional interface bounds, throws or changed runtime slot.
Full substituted parameters, results and bound arguments must agree by formal
position while class scopes stay distinct. The plain nominal-bound profile's
unchanged owned interface JAR and JVM oracle retain five input classes and six methods, compare 53 identities
and four failures, and check nullable/raw storage, subclass dispatch and bound
reflection. Changing Bound to OtherBound only in the implementation's Signature
keeps its original Pair-to-Pair ABI and Code but is rejected by NeverD and
javac. Coverage is `recovered`; the Object-bound oracle remains separate.

Inherited method bounds may also depend on the original class formal and on
another method formal, such as `<A extends T, B extends A>`. Full alpha-equivalent
comparison preserves each declaring scope, array position and callback wildcard.
An alternate single interface-slot encoding of the same variable bound uses the
existing `type-variable-bound-position` report; its source coverage stays
`partial`, while ordinary Java encoding stays `recovered`. The owned source and
immutable dependency-JAR profiles retain three classes/six methods or one
class/three methods. Each checks 352 identities, 176 state observations and 24
ordered failures through direct, raw, reflection, method-handle and independently
compiled caller paths. Generic reflection, complete metadata and exception
inventories remain matched except the disclosed bound position. Same-Code
changes to a different class formal or a different dependent bound fail; an
independent javac counterexample also fails. Generic invocation still requires
its separate existing proof.
The same bounds are checked when the implementation is inherited from an
original class provider. Superclass substitutions retain each class-formal
owner before all method bounds, parameters and results are compared. Source and
immutable provider-JAR fixtures cover ordinary and interface-slot encodings,
retaining four classes/six methods or the single input subclass/constructor.
Each checks 352 identities, 176 states and 24 failures with original arrays,
callbacks, exceptions, complete generic reflection and metadata. No declaration
is copied into the subclass. Same-Code dependent bound drift must fail both
source and dependency recovery; independent javac rejects the incompatible
inherited implementation. Encoded input declarations retain the existing
reported bound-position difference; unchanged dependency declarations do not
create input method projections.
Dependent class formals use the same encoded-bound receipt during superclass
composition. A class declared with `U extends T` may retain that relation when
its input Signature places U's sole bound in the interface slot. Source and
immutable provider-JAR oracles cover both encodings, retain four classes/six
methods or one subclass/constructor, and check 352 identities, 176 states and 24
failures. Class-bound reflection, original providers, complete metadata, callbacks
and aliases stay intact except the single reported class-bound spelling change.
Changed method bounds and a different superclass formal binding fail even when
the variable erasures agree. Ordinary encoding remains `recovered`; alternate
class encoding remains `partial` for its explicit Signature projection.

One method-owned plain class bound may also support original covariance among
unary parameterized return types. The common subtype validator proves complete
result ancestry with exact method-formal substitution; every parameter and
bound still has to match. Original class and interface bridge bodies use their
exact virtual or interface dispatch, with optional proven result upcasts.
Inherited input-interface bridges require their own complete proof regardless
of class order. If only a narrower abstract interface declaration exists in
DEX, `interface-covariant-runtime-bridges` retains that original declaration and
reports the additional javac slots as `compiler-covariant-interface-bridge`
helpers, including target, dispatch and flags. Declared-method/default/bridge
reflection, rebuilt dispatch and stack frames may differ; coverage stays
`partial`. The two owned D8/JVM oracles retain ten classes and twelve or eleven
original methods, compare 103 identities and six failures, keep original class
bridges and generic bound/scope reflection, verify the exact disclosed extra
method inventory, and reject a same-erasure generic ancestry change with
unchanged Code in both NeverD and javac.

A zero-argument abstract interface return may also narrow a complete generic
result to a known nominal type without an original method Signature. Its known
ancestry must satisfy every substituted result argument; runtime erasure alone
cannot prove covariance. The original plain return declaration is retained,
and javac's additional default bridge is disclosed through the same partial
coverage report. The owned oracle preserves the original interface inventory
and concrete implementation Code, compares 24 identities and three failures,
and checks generic ancestry reflection and unchanged class dispatch. Changing
only the nominal result's ancestor argument is rejected by NeverD and javac.

The enclosed-formal oracle retains seven classes and all seventeen method descriptors, checks 75 identities and eighteen ordered failures, and confirms exact enclosing method ownership despite a same-name class formal. Captured field identity, independent instances, superclass convenience overloads, bounded bridge casts, null captures and exception identity remain matched. Source uses Java 25 to keep captured field writes before the original superclass constructor. `anonymous-to-named-local` reports the changed binary names and anonymous/local reflection; method formals, bounds and generic member reflection remain intact. An enclosing target parameter Signature mutation with identical Code and descriptors is rejected by NeverD and javac. Coverage stays `partial`.

An original covariant bridge may contain one post-call cast of its unchanged typed result to the exact erased bridge return type. The common nominal assignability proof must establish that this is an upcast; the complete generic result contract is checked separately before forwarding. Null and object identity remain unchanged. A different target, receiver cast or duplicate cast fails. An independently authored compiler bridge receives one recorded `CHECKCAST Base` Code edit, while its flags, descriptors, signatures and all other methods remain unchanged. Its D8/JVM oracle compares 19 identities and seven failures, including virtual overrides, target effects and exception identity, retains generic reflection and rejects a same-size changed cast. Regenerating that original bridge keeps coverage `partial`.

The shared value-carrier proof also accepts a result-only conversion: unchanged Object-erased inputs, an original instance target returning long, and the exact known static boxing factory. No input cast or unboxing call is invented. A scoped input requires the complete original target Signature to agree in declaration scope; missing metadata and another formal with the same erasure fail. The independently authored D8/JVM oracle retains original Code and descriptors, compares 126 values, 131 identities and eight failures, and checks factory allocation, mutable values, null inputs, target failure before boxing and unchanged typed calls. Changing only the target's Signature from S to V is rejected. The raw interface and bridge flag differences remain reported, with coverage `partial`.

The same proof accepts a differently named reference target whose result
already matches the complete original interface contract. It verifies the
single original virtual call and return, retains both runtime method slots,
and adds no cast, unboxing or boxing. Scoped parameter and result Signatures
must agree with their original declaration scope. Its independent D8/JVM
oracle checks 1,260 values, 1,263 identities and six ordered failures, including
null results and inputs, mutable returned carriers, exact exception identity
and one target call per dispatch. Complete class, field and method metadata
stays unchanged except the reported raw interface edge and bridge flags;
three same-erasure scope changes with unchanged Code are rejected. Coverage
remains `partial`.

A final Object-based implementation of a fixed interface may have an original
Object-returning method whose unary consumer parameter is `Consumer<? super
Value>`, while the inherited slot declares `Consumer<Object>`. The shared type
validator proves that every inherited parameter value satisfies the original
body contract before projecting only that parameter's source Signature.
`inherited-contained-consumer-parameter` retains the full original Signature
and inherited witness; runtime descriptors, method bodies, result and field
metadata stay unchanged. This bounded profile requires a known plain lower
class and one Object-bounded interface formal. Other variance, open bounds,
missing target metadata and nonfinal implementations remain unproven. Its
independent D8/JVM oracle checks 252 values, 284 identities and eight ordered
failures, including callback virtual dispatch, exact exception objects, nulls,
mutations and target failure before callback execution. It independently
confirms that javac rejects the unprojected override, compares all metadata
against the report and rejects three same-erasure variance counterexamples
with unchanged Code. Coverage remains `partial`.

The same containment receipt handles an original `Consumer<?>` or canonical
`Consumer<? extends Object>` parameter when the inherited slot requires
`Consumer<? super Value>`. It proves the inherited-to-original relation before
changing source metadata and retains every original body operation. Named
static member interfaces use the same independent-scope predicate as the
bridge validator. A second owned D8/JVM profile uses a static member interface,
checks 252 values, 284 identities and eight ordered failures, preserves the
original wildcard field, and compares complete metadata while rejecting three
same-erasure variance changes. Both original override forms are independently
rejected by javac. Open or parameterized lower bounds and method formals do
not enter this bounded profile.

`test_mobile_android_inherited_contained_consumers.py` extends the unbounded
consumer proof across generic interfaces, transitive and repeated interface
paths, a generic superclass and inherited concrete providers. The common
validator normalizes source ancestors first and requires agreement of every
complete inherited contract. Original class formals and other method parameters
stay scoped; only the two reported unbounded consumer parameters become the
required closed lower wildcard. The independent JVM caller checks direct,
interface, subclass, reflection and MethodHandle dispatch, nullable mutable
results, exact exceptions, erroneous callbacks and effects before failure.
Complete metadata and Exceptions change only at the reported method Signatures.
Same-Code partial input, upper output and method-formal mutations are rejected;
independent javac also rejects the unprojected wildcard override. Source and
unchanged declaration-JAR profiles retain original runtime ownership and keep
coverage `partial`, with no unrecovered methods or generated bridges.
The source profile retains eight classes and eight methods, and the SDK profile
four classes and four methods. The oracle compares 144 values, 264 identities,
416 states and 80 failures.

A complete generic erased bridge may use `Consumer<Object>` in its typed
method while the inherited specialization requires `Consumer<? super Object>`.
The common closed-parameter proof permits this exact known Object lower bound
and records `inherited-closed-parameter-variance`; it does not treat Java
wildcard capture as type equality. It verifies every other complete parameter,
the result and the original bridge before changing only source parameter
metadata. The independent D8/JVM oracle checks 504 values, 508 identities and
ten ordered failures, including the original callback's erased generic cast
failure. All class, field, result, method and bridge metadata remain unchanged
except the reported typed parameter Signature. Three same-erasure bounds
counterexamples fail with unchanged Code. The existing final-class parameter
projection and mixed carrier proof reuse the same closed-bound decision.
Coverage remains `partial`.

Checked reference inputs may keep complete parameterized types while another original input undergoes a known primitive conversion. The original target Signature must agree with every scoped parameter and complete result; its runtime descriptor alone is insufficient. Parameterized or open input carriers cannot authorize an unboxing operation through this profile. The independent generic-input D8/JVM oracle retains five classes, eight methods and original Code, compares 162 values, 491 identities and sixteen ordered failures, checks nullable reference identity, integer edges and effectful Number overrides, and retains complete target generic reflection. A target input change from Box<S> to Box<V> with identical erasure and Code is rejected. Source-interface and bridge flag differences remain reported with coverage `partial`.

The same exact result-boxing proof accepts a zero-argument invoke contract.
The owned D8/JVM oracle retains four classes, eight original descriptors and
all original Code, compares 21 wide values and eight ordered failures with five
exception-identity checks, and verifies separate carrier allocations and typed
calls without added boxing. Changing the declared interface result to String
with unchanged Code fails. Original factory provenance, wide pairs and call
order are checked; coverage and metadata differences stay reported as above.

A closed Boolean result can use the same original boxing proof. The final
owner must retain a public synthetic bridge that calls the exact primitive
boolean target, moves its one-word result, invokes the known
`Boolean.valueOf(boolean)` factory and returns that original boxed result.
Complete interface arguments and target Signatures are checked before any
source projection. A same-name primitive return slot receives one disclosed
source alias shared by its declaration and known callers; the Object bridge
keeps its original boxing call. An additional inherited primitive contract
prevents this alias because renaming only the implementation would change
dispatch. Unknown ancestors, another boxed result, another factory, changed
receiver/result provenance and altered bridge flags remain unsupported.

The Boolean-result source and `--dependency-interface` JVM profiles compare
primitive, erased and reflective calls, mutable state, target effects and
failures, null receivers and `Boolean.valueOf` cache identity. The source
interface edge is explicitly raw: reports retain its original complete
Signature, the original bridge witness and the primitive method's source name
and identity. Generic-interface reflection, primitive name/binary ABI and
bridge/synthetic flag differences remain disclosed with coverage `partial`.

Standard Long inputs retain the original Number cast and virtual `longValue`
call, its wide register pair, and the exact target's scoped reference result.
The authored D8/JVM oracle compares long edge values, non-Long Number
implementations, effects, nullable result identity and ordered failures.
Carrier forwarding is proven before a shared local class's enclosing formals
are projected for source emission, so different formals with the same Object
erasure and missing target Signatures remain rejected. Source metadata and
bridge flag differences remain reported with coverage `partial`.

Standard Double inputs use the same conversion table and preserve the exact
original `Number.doubleValue()` call, including custom Number implementations.
Its result occupies two consecutive registers at the original target call;
reference result identity and the complete target Signature remain unchanged.
The authored oracle preserves three classes and five methods, compares 108
values, 113 identities and twelve ordered failures, and checks raw double bits,
signed zero, infinities, NaN payloads, conversion effects and full metadata.
A same-erasure result-scope change fails. The raw interface and bridge flags
remain reported with coverage `partial`.

The same conversion table recognizes the exact known `Number.floatValue()`
declaration for Float inputs. Complete generic reference inputs keep their
original variance and checked identity, while primitive and result register
provenance must match every original instruction. The authored JVM fixture
checks raw float bits, signed zero, infinities, NaN payloads, non-Float Number
overrides and exception order; target-only generic variance drift is rejected.

A final source class may keep an original bridge to a differently named
generic superclass setter. The complete target parameter must match the
inherited scoped formal, then the original unchanged-receiver virtual call and
void return are proven. Both method names, descriptors and bodies remain;
`named-generic-setter-bridge` reports changed bridge/synthetic flags. The owned
JVM oracle checks direct and inherited calls, null and object identity, effects,
generic reflection and exception identity, and rejects another formal with the
same Object erasure and unchanged Code.

An invariant reference argument nested in a bridge target parameter can be
projected to the exact inherited `? extends` argument only for the same known
plain final class. All other nested arguments, declaration scopes, input and
result types must match, and the original bridge's checked forwarding must be
proven first. `inherited-final-parameter-variance` discloses the changed target
Signature; the full interface and runtime descriptors remain intact. The owned
JVM oracle checks input, field and result identity, callback effects, nulls and
exception order, and rejects a different final class with unchanged Code.
The full inherited member contract also covers repeated class formals and an
unchanged primitive int result, including Comparator-style methods. One
complete substitution supplies every expected parameter, result and scope.
The Comparator JVM oracle checks 90 integer values, 182 identities and six
ordered failures, retains full nested list bounds and all other metadata, and
rejects changed final leaves, lower bounds and nested argument drift.
An original invariant result may satisfy the same closed final-bound
covariant result while only parameters receive this projection. The separate
List JVM oracle checks 1,202 identities, 406 effects and six failures, including
unchecked erased leaves. It compares all original metadata, retains return
and field Signatures, and rejects changed result leaves, contravariant inputs
and open result formals.

Mixed reference/wide value-carrier bridges use the same final-bound decision
for unchanged closed reference parameters. Their already-reported raw interface
edge permits the original target Signature to remain invariant. Primitive
carrier parameters and results still require exact original contracts. The JVM
oracle compares 320 values, 963 identities and nine ordered failures, retains
the complete target metadata, and rejects another final leaf, a contravariant
bound and an open formal with unchanged Code and descriptors.

A final class can retain an original unary Integer consumer with distinct
`accept(int)` and erased `accept(Object)` entries. The complete interface
contract must substitute its Object parameter to Integer and return void. The
common bridge proof preserves the original Number cast, virtual `intValue`
conversion, one-word result, unchanged receiver and primitive target call.
A plain `(I)V` target needs no invented Signature; a present Signature must
still pass full parameter, result, method-formal and exception checks.
The consumer's own interface edge becomes raw, while the shared generic
interface declaration and both original method names, descriptors and bodies
remain unchanged. No boxed Integer cast, boxing or Unit result is added.

The integer-void-consumers source and `--dependency-interface` JVM profiles use
a named nonstatic captured receiver. They verify primitive and erased calls,
non-Integer Number dispatch, mutable state, conversion and target failures,
null and incompatible inputs, and unchanged shared interface metadata. The
existing capture proof retains constructor order and reports its static-member,
capture-field and reflection differences. Preserved constructor prologues need
Java 25. Raw interface and bridge/synthetic flag differences remain reported
with coverage `partial`.

The `--inherited-interface` variants apply the same proof to a consumer that
redeclares its callback above known generic parent interfaces. Every generic
ancestor has one Object-bounded formal. An ancestor declaring a callback must
substitute it to Integer and retain the same public abstract Object-to-void
contract. Generic markers may use other complete closed arguments, but every
path to the same marker must supply the same argument. The bounded graph checks
exact invariant edges, declaration scopes and repeated ancestors before the raw
source edge is permitted. Conflicting marker arguments, cycles, missing
declarations, default methods and extra members fail. Source and
dependency-interface variants retain the original parent declarations and
generic Signatures.

A separate integer/Unit bridge proof retains both complete original methods and their runtime names. A final class must supply a direct Object-bounded `invoke` contract with plain reference arguments or exact scoped formals, a known Integer-to-Number hierarchy and public instance `Number.intValue() -> int` declaration, the exact primitive void implementation, and the known Unit result field. The bridge may only copy unchanged inputs, check its reference arguments in order, unbox each corresponding integer once with its original virtual call, delegate and read Unit after success. `checked-integer-unit-interface` erases the implemented interface arguments in source after validating the original bounds, and the common validator rechecks dependent source bounds. `integer-unit-interface-bridge` emits the original body as an ordinary Java method; generic interface reflection and bridge/synthetic flags change explicitly. No unboxing or Unit read is added to typed void calls. Projection reports retain the original class-interface Signature substring verbatim; scoped type comparison keys are never serialized as JVM metadata. The D8/JVM oracle retains all five classes, eight method descriptors and Code bytes, compares 23 identities and ten ordered failures, checks non-Integer Number implementations, integer edge values, mutable state, getter dispatch and Unit initialization, and confirms the reflection differences. Coverage stays `partial`; primitive ABIs other than the separately proven integer and boolean conversions remain unproven.

The same shared primitive conversion proof supports Boolean.booleanValue with its exact known public final Object-based Boolean declaration. Only the original checked Boolean receiver may be unboxed, and its boolean result must immediately flow to the matching primitive void parameter. Remaining reference parameters retain their complete resolved scopes, including repeated K/V arguments; an original target Signature must agree in every consumed position and cannot introduce method formals or generic exceptions. `checked-boolean-unit-interface` and `boolean-unit-interface-bridge` report the raw interface and bridge flag differences while preserving both original bodies, names and descriptors. The independent four-parameter JVM oracle retains four classes and seven methods, checks 20 identities and seven ordered failures, verifies true/false forwarding, wrong-type and null failures before target effects, target failure before Unit initialization, exact Unit identity and unchanged typed generic parameter reflection. A same-erasure formal change with identical Code is rejected. The integer oracle remains separate and confirms its existing virtual Number dispatch behavior.

A final class may implement an exact generic Unit-returning interface contract through an original `void` target and checked erased bridge. The full bridge must check its unchanged incoming arguments, delegate once, read the known public static final `kotlin.Unit.INSTANCE` field after success, and return that exact value. A distinct Java name preserves the complete void body and every typed input call without adding a Unit read; inherited void overrides, incompatible inherited slots, missing declarations and extra effects remain unproven. The typed Unit wrapper emits the original bridge body and Java regenerates its erased descriptor. `void-unit-target` and `void-unit-interface-bridge` record both original identities, source identities, the result field and changes to method reflection and stack frames. Names avoid original and inherited methods. The D8/JVM oracle retains all owned Code bytes and input descriptors, compares 27 identities and nine ordered failures, observes Unit initialization after the first successful bridged call, and confirms the reported reflection change. Coverage stays `partial`.

That checked Unit profile also permits original generic parameters on the void target. The complete target Signature must retain the exact erasures and class-owned scope, with no method formals or declared exceptions. A contained argument may use the exact same upper/lower wildcard bound or an unbounded wildcard; ordinary override parameters still require complete equality. The new Unit wrapper uses the bridge's checked runtime parameter erasures and records that metadata difference, while the renamed void target preserves its original Signature. A separate D8/JVM oracle checks 20 identities and twelve ordered failures: callback effects, casts before target effects, callback result casts and Boolean unboxing, exceptions before Unit initialization, and typed void calls without added Unit reads. It confirms the target's generic reflection, the raw wrapper parameter, and the recorded rename; an incompatible generic parameter with unchanged Code and descriptors is rejected. Coverage stays `partial`.

The ordinary reference-parameter Unit proof does not depend on the callback
being named `invoke`. It uses the exact original contract and forwarding body
for names such as `apply`, including a class-owned formal and another checked
reference parameter through a generic superclass. All known ancestors must be
available to exclude an inherited void override or incompatible slot; Object
member names stay outside this bounded rename proof. The source name avoids
both input and inherited declarations. The source/parent-JAR oracle checks
seven call routes, mutable generic aliases, casts before side effects, failure
identity, and normal plus failed Unit initialization in fresh JVMs. Original
method and class metadata remain intact except the reported void-name mapping
and added Unit wrapper; the original erased bridge is regenerated. Same-Code
bridge-flag and method-formal counterexamples are rejected, and independent javac
rejects the same class-versus-method scope conflict. Each profile compares 14
values, 383 identities, 290 state observations and 85 ordered failures. The
source profile retains five classes and eleven methods; the parent-JAR profile
retains three input classes and eight methods. Coverage stays `partial` with no
unrecovered input methods.

Direct generic contracts may inherit known empty marker interfaces that extend only `Object` and have no fields, methods or enclosing declarations. A marker may also have an acyclic bounded ancestry of plain empty marker interfaces; generic edges within that marker ancestry require a separate argument-composition proof. The complete generic inheritance declarations and argument bounds remain validated. A formal is substituted only when an inherited method consumes it; scalar members and empty markers retain otherwise unused open arguments without an invented erased binding. Unproven open scopes, nonempty or unknown marker parents and incorrect bridges still fail. The independent javac/D8/JVM oracle retains seven classes and all eleven runtime method descriptors, checks 31 identities and seven ordered failures, and confirms marker, unused-formal and raw-superclass reflection. Bridge regeneration keeps coverage `partial`.

The same bridge proof supports a direct generic superclass extending `Object`, with only those empty marker interfaces and at least one original constructor. Public override contracts use `generic-superclass-bridge`; inherited concrete methods without overrides stay in their original class, and constructors are not treated as inherited methods. This includes checked forwarding to a `void` target. More complex inheritance, unproven open scopes, generic method invocation and hidden constructor parameter mapping remain explicitly unsupported where the required proof is unavailable.

A concrete or source abstract generic superclass may also retain a fully known nongeneric superclass/interface graph. Every ancestor must have no class or method Signature, preserve its class/interface role and have an acyclic bounded graph. Inherited abstract contracts need an exact public concrete class implementation selected from the nearest class declaration; an abstract or inaccessible nearer member cannot be bypassed. A source abstract generic parent may defer a plain ancestor contract to the child. That provider must retain the complete plain parameter and result types, with no method formals, bridge, static kind or added exceptions; the generic parent’s own contracts still receive the common full substitution proof. A child override of a higher slot must either match a direct parent slot covered by full substitution, or retain the exact public plain ancestor contract. The latter permits no method formals, additional declared exceptions or regenerated bridge; any original Signature must match every complete plain parameter and result type. Static/instance kind and final constraints remain checked. Interface defaults cannot supply that abstract-contract proof; unknown ancestors, generic higher edges and unproven overrides still fail. Source keeps the original inheritance declarations and bodies, while the existing full substitution and bridge checks cover the direct generic parent. The owned javac/D8/JVM oracle retains five classes and twelve method descriptors, compares 25 identities, eight ordered failures and 32 superclass view calls, and confirms mutable inherited state, interface dispatch, generic reflection and subclass overrides, including a higher ancestor method not redeclared by the generic parent. A target Signature using a different same-erasure formal is rejected by NeverD and an independent javac override check. The abstract-parent profiles retain abstract generic and ordinary ancestor declarations, compare every original class and method attribute, and reject a nearer private provider with unchanged Code. They retain thirteen methods (fourteen with the parameterized interface) and the same runtime counts. Bridge regeneration keeps coverage `partial`.

A generic superclass contract may also be `protected` when the specialized implementation and original synthetic bridge are both `public`. Complete substitution, original forwarding-body checks, final constraints and unchanged exceptions still apply. The original protected parent declaration and public promotion remain in source; erased calls requiring a separate protected receiver-access proof fail explicitly. Protected implementations requiring a new bridge, package-private contracts and changed bridge visibility are outside this profile. The independent cross-package javac/D8/JVM oracle retains five classes and eleven method descriptors, regenerates two public bridges, checks 31 identities and eleven ordered failures, and confirms generic reflection, varargs flags, mutable state and subclass dispatch. It covers both a checked parameter-array bridge and a return-only bridge with unchanged parameter descriptors. A different same-erasure parameter formal with unchanged Code and descriptors is rejected by NeverD and javac; coverage stays `partial`.

An independently scoped abstract input class extending `Object` may inherit public abstract generic interface requirements without redeclaring them. The common inheritance planner checks complete substituted parameters and results across both erased and specialized slots. Conflicting class formals, existing competing methods, method formals, generic throws and projected source scopes do not use this proof; concrete classes still need an implementation. Fixed declared exceptions consume the common validator’s closed Throwable receipt, and a deferred abstract superclass declaration must have the same proven exception compatibility key. No inherited declaration, implementation or bridge is invented. Existing generated exception helpers are separately reported and checked. The independently authored javac/D8/JVM oracle covers input and dependency interfaces, exact original method inventories, subclass dispatch, mutable state, 30 values, 216 identities and 30 ordered exceptions. A Signature-only conflict between distinct Object-bounded result formals leaves Code and descriptors unchanged and is rejected independently by NeverD and javac.

The fixed-exception profiles preserve every original class and method attribute, including exception declarations and an array of a class formal. They compare 30 values, 230 identities and 36 ordered failures (32 values and 231 identities with a class ancestor), including six actual checked exceptions with unchanged objects and state before failure. No interface implementation or bridge is added to the abstract holder. The same profiles cover source and SDK interface/ancestor partitions, reject the original same-erasure scope conflict, and reject a non-Throwable exception declaration with unchanged Code. Generic exception formals, unknown hierarchies and missing validation receipts remain unsupported.


A separate final-owner profile preserves an original public erased getter that
forwards to a protected, Signature-free, no-argument getter with a closed plain
reference result. Full superclass substitution and the common forwarding proof
must agree. The typed getter becomes public in Java so javac keeps the original
public erased entry; `public-erased-protected-getter-target` discloses that
access change. Names, bodies, descriptors and generic contracts remain intact.
The ThreadLocal JVM oracle checks 965 values, 965 identities and six failures
through cached, cleared, typed and public erased calls, compares full metadata,
and rejects changed inherited arguments and a conflicting protected source descendant.

The same public-entry proof permits a known nominal class erasure instead of
`Object`. Full inherited substitution must still provide the closed plain getter
result, and the common assignability proof must certify its relation to the
erased entry. An original redundant result cast is checked against that same
relation. The recursive self-bound source and dependency oracles compare 845
values, 604 identities and five failures, preserve the complete recursive bound
and all original metadata except the reported typed access, and reject changed
complete arguments or a conflicting protected source descendant. The original ThreadLocal case retains
its separate oracle. Coverage stays `partial`.

`test_mobile_android_protected_open_getters.py` verifies the public erased
getter on an independently scoped nonfinal class. Its typed protected result
is a nominal subtype with complete generic ancestry, and an input child inherits
the original slots. The shared access proof rejects known source descendants
that redeclare the typed getter. Only source target access becomes public;
original descriptors, class scopes, fields and bodies remain unchanged. Source
and immutable parent-JAR profiles check nullable mutable results, fresh
allocations, effects before failure, parent callbacks, reflection, MethodHandles
and an independently compiled protected override. Each profile compares 90
values, 432 identities, 954 state checks and 114 ordered failures. The source
profile retains five classes and ten methods; the parent-JAR profile retains
four input classes and seven methods. Complete metadata changes only at the
explicitly reported target access. Same-Code bridge flags and incompatible
parent arguments fail recovery; independent javac rejects the wrong result
contract. Coverage stays `partial` with no unrecovered methods.

`test_mobile_android_protected_array_results.py` verifies the corresponding
int-parameter, covariant reference-array result. An inherited bounded array
contract and original public erased bridge select the exact protected target.
The source target becomes public, reported as
`public-erased-protected-array-result-target`; no runtime descriptor or body is
replaced. Source and immutable parent-JAR profiles compare null and cached
arrays, fresh allocation and zero initialization, mutable element aliases,
actual component types, ArrayStoreException, negative lengths and effects before
failure through six routes and an independently compiled protected override.
Each profile checks 486 values, 3204 identities, 3456 state observations and
672 ordered failures, including an ordinary array upcast body. Source recovery
retains five classes and eleven methods; the parent-JAR profile retains four
input classes and eight methods. They compare complete class/method/field
metadata and reject unchanged-Code bridge-flag and incompatible generic-parent mutations. Independent javac rejects
the incompatible result contract. Coverage remains `partial` with no unrecovered
methods or invented descendant methods.

A final owner's protected target can also retain a closed generic result and
closed generic parameters behind its original public erased bridge. One shared
access predicate consumes the validated original Signature; complete inheritance
comparison still checks all arguments and result covariance. The
`public-erased-protected-generic-result-target` report records the public source
access, while an anonymous owner keeps its separately disclosed named-member
binding. Source and immutable parent-JAR oracles compare nullable returned
functions, mutable aliases, callback effects, failure identity and generic
reflection through six routes. Full original metadata is checked under those
two explicit changes. Same-Code bridge flags and incompatible generic parameter
or result arguments fail recovery; independent javac rejects the incompatible
result contract. Each profile compares 36 values, 540 identities, 366 state
observations and 132 ordered failures. Source recovery retains five classes and
ten methods; the parent-JAR profile retains four input classes and seven methods.
Four method projections disclose the access/bridge and anonymous construction
mappings. No method formal or open result is admitted by this profile.

A Signature-free concrete target may omit the fixed declared exceptions of its
original erased bridge. The common forwarding proof requires the bridge's
complete ordered Exceptions attribute to match its exact inherited runtime
contract and uses the validator's closed Throwable receipt. A missing, changed
or reordered entry cannot be hidden by the empty target declaration. Generic
exception formals and targets with a different nonempty exception set need a
separate proof. The independent source and dependency JVM oracles exercise SDK
Callable and an owned checked-parameter interface, compare 31 values, 36
identities and 28 failures, verify the original exception order on interfaces
and bridges and empty throws on concrete targets, and preserve full original
metadata and inventory except reported helpers. Five unchanged-Code scope and
exception counterexamples fail before output. Coverage stays `partial`.

A protected generic superclass contract may retain its original protected typed
array/result or void callback implementation after complete substitution changes
the runtime slot. Its original bridge must have exactly protected/bridge/synthetic
access in the same known non-interface owner, and the common register-provenance
proof must verify every checked argument, self invocation and unchanged result.
This does not promote either method or permit a protected implementation of a
public contract. Source and dependency-parent JVM oracles retain all three
original bridges, check 66 values, 45 identities and 24 failures, preserve mutable
outer capture and callback dispatch during superclass construction, and compare
complete class, field and method metadata except the reported capture flags and
helpers. Same-erasure scope, complete inherited arguments, public contract and
bridge flag mutations with unchanged Code are rejected. Coverage stays `partial`.

An input generic parent extending `Object` may implement known parameterized interfaces when its complete interface-member proof passes in the same common inheritance plan. Child and parent processing order cannot bypass that proof; opaque parents, raw nonempty contracts and missing implementations still fail. A protected child override may preserve a protected parent slot when every substituted runtime parameter and result descriptor stays identical and the full generic types match; this case introduces no bridge. An untouched external default remains inherited through known nonempty direct interfaces and their bounded linear external ancestor chains, with any additional interfaces proven to be empty markers and no competing source class members. Every class in the superclass chain must be known input, with no same-name member or additional nonempty interface, and the chain must end at `Object`. An explicit external default override can retain its original public implementation when complete substitution matches, parameter/result runtime descriptors do not change, no declared exceptions are introduced, and no bridge is needed. Competing or otherwise unproven default graphs require separate proofs. The independent cross-package javac/D8/JVM oracle uses API 24 to retain native interface defaults, places the unchanged independently authored interface and tracing class in a declaration JAR, and recovers five input classes and twelve method descriptors. It checks 43 identities, seven ordered failures, one regenerated public bridge, protected virtual dispatch, default effects before bridge casts, generic reflection, mutable state and integer overflow. A different same-erasure protected parameter formal with unchanged Code and descriptors is rejected by NeverD and javac; coverage stays `partial`.

Untouched external defaults use the unique most specific public instance declaration of the complete unchanged runtime slot across the known interface DAG. A nearer abstract declaration cannot be bypassed, and an inherited more-derived default keeps its original binary implementation. Every provider interface must be known external, Object-based and independently scoped; unchanged source intermediate interfaces use the separate proof below. Reachability resolves redundant ancestors and disjoint branches; every maximal matching declaration must select one concrete provider. Cycles, unknown declarations, multiple unrelated maximal slots, incompatible same-slot returns, source-owned defaults and class competitors remain unproven. Unrelated overloads retain their own bodies; candidates matching either the erased or fully substituted parameters still require the original override/bridge proof, including inherited source-class candidates. The independently authored declaration JAR contains three generic interface levels with a redundant ancestor diamond, a marker and mutable tracing state. Recovering and recompiling its parent and child input implementations preserves all four original descriptors and generic reflection. The JVM oracle checks 43 identities and sixteen ordered failures through class and interface views, verifies the nearest override, a root-only default, a generic reference default and covariant self returns, and observes dependency effects before exceptions. A target Signature that moves a class formal into a method scope with unchanged Code and descriptors is rejected by NeverD and javac. Coverage is `recovered`.

A known class ancestor without class or method Signature may be
source or an unchanged dependency. Default selection uses the common plain
ancestor graph proof including that original parent, while retaining complete
interface substitution and unique default-provider selection. Unknown or cyclic
ancestors, abstract dependency parents, competing slots and invalid static/final
or plain overrides fail. An abstract source ancestor retains the existing proof
when the child supplies every required plain concrete implementation. The source and dependency JVM oracles preserve SDK
Consumer.andThen through AtomicBoolean and an owned plain parent. They compare
100 values, 56 identities and 24 failures, retain atomic state, callback ordering
and exception short-circuiting, confirm Consumer remains the default's declaring
owner, and verify full original metadata and inventory except reported helpers.
Same-erasure formal drift and a final ancestor override with unchanged Code are
rejected in both modes. No inherited method or bridge is invented; coverage is
`recovered`.

An external default may pass through known source interfaces that declare neither its erased nor its fully substituted parameter slot. The common inheritance planner still checks every complete generic edge, including redundant diamond paths and distinct formal scopes. Source intermediates cannot replace the provider, and projected source scopes or competing source slots remain unproven. The owned oracle recovers four classes and two original methods, preserves the external dependency Code and default declaring owner, compares 44 identities and four failures, and checks virtual overrides, null fallback identity, ordered effects and full generic reflection. Changing only one redundant ancestor argument with unchanged Code and descriptors is rejected by NeverD and javac. No inherited declaration or bridge is added; coverage is `recovered`.

A source interface may contain a JVM method returning its own complete parameterized type beside an external same-parameter method returning a primitive, void or an unconstrained ancestor class formal. Java cannot spell these distinct return slots. The shared inheritance plan assigns a collision-free source name to that input declaration, its known implementations and resolved callers, while retaining the original full generic member checks and external default provider. `incompatible-inherited-return-slot` records the original inventory, source identity and inherited conflicting contracts; the original runtime method name, binary ABI and name reflection cannot be regenerated. The independently authored oracle constructs only recorded UTF8 method-name edits with unchanged Code and descriptors, recovers five classes and sixteen methods, and compares 66 identities and six failures. It checks interface and virtual dispatch, input call bodies, source-name collisions, generic reflection, mutable state and unchanged external default effects. A method-formal scope change with the same Code and descriptors is rejected by NeverD and javac. Coverage stays `partial`.

A package-visible generic class contract may likewise retain its original
package-visible typed method and synthetic bridge after full substitution
changes a parameter or result descriptor. The declaring parent and child must
remain in the same package. One common access proof admits only the original
bridge/synthetic flags and a concrete, nonstatic package-visible target in the
same known non-interface owner; the existing complete cast, argument, receiver
and return provenance proof still checks the body. Public/interface contracts,
cross-package ancestry, private targets and changed bridge flags fail. The
`--package-private` source and dependency-parent JVM profiles retain all three
original array/result and void bridges, compare 66 values, 45 identities and
24 failures, verify mutable captures and pre-super constructor effects, and
check full metadata except recorded capture flags and helpers. Five
unchanged-Code type, scope, visibility and bridge-flag counterexamples fail.
Coverage stays `partial` without promoting any method.

A class without a class Signature may still retain complete generic member
Signatures when default selection traverses its known ancestry. The common
scope validator binds dependency members along that exact known plain edge;
ordinary and dependency declarations use the same resolved contracts. The
method-contract comparison preserves constructor metadata and unrelated generic
getters or method formals, proves each required concrete implementation, and
compares every overriding method's full types and method-formal bounds.
Member source projections, same-erasure argument or scope drift, inaccessible
implementations and competing default slots remain rejected. The
`--member-signatures` source and dependency-parent JVM profiles preserve the
original default owner, constructor and getter metadata, method formals,
callback order and mutable state. They compare 133 values, 101 identities and
24 failures, check the complete original inventory except reported helpers,
and reject four unchanged-Code metadata counterexamples. No inherited method
or bridge is added; coverage is `recovered`.

The shared complete-method comparison distinguishes interface contracts from
plain class overrides. Interfaces and public class methods still require a
public implementation; protected declarations permit public or protected
overrides, and package declarations permit a nonprivate override in the same
package. Full parameters/results, alpha-equivalent method-formal bounds,
static/final constraints and absence of added exceptions remain checked.
The `--nonpublic-methods` source and dependency profiles keep the original
package visibility and SDK default owner, compare 140 values, 111 identities
and 24 failures, and verify dynamic dispatch through the parent with mutable
method state. Every original class and method attribute remains unchanged;
seven unchanged-Code type, scope, final and visibility counterexamples fail.
No bridge or inherited declaration is added; coverage is `recovered`.

An abstract source superclass can leave a plain interface callback undeclared
when its child supplies the original public concrete instance method. The
shared complete-method proof compares every parameter and result, including
alpha-equivalent method formals and their full bounds. This admission applies
only to an abstract interface requirement; existing superclass declarations
retain their normal static/final, visibility and override checks. A missing,
abstract, static, private or bridge implementation, different complete scope,
or concrete parent missing its own required implementation still fails.
No callback declaration, bridge, body or source Signature is invented.

A plain override may narrow a fixed interface exception declaration to an
empty throws clause. The interface's original exception list must have the
common validator's closed Throwable proof; full parameter/result types and
method-formal bounds still have to match. The same rule applies to source and
SDK interfaces, whether the original implementation belongs to the generic
parent or its concrete child. Each declaration keeps its own original
`Exceptions` metadata; recovery adds no exception to the implementation and
creates no Signature projection. This profile does not admit nonempty
implementation throws or generic throws, and existing bridge/inherited
exception-set equality proofs remain unchanged.

The fixed-throws-empty-overrides source and `--dependency-interface` JVM
profiles compare child, parent, interface and reflective dispatch, callback
casts before target effects, mutable state, null inputs and ordered exception
identity. They check the original class and method inventory and all metadata,
including the interface's fixed exceptions and the parent's empty declaration.
Original compiled inputs, harness, SDK and dependency declarations remain
immutable. Same-Code scope and non-Throwable counterexamples fail, alongside
independent javac scope and checked-exception counterexamples. Regeneration of
the fixture's original generic bridge is separately reported with coverage
`partial`; the throws narrowing itself adds no source projection.

An original concrete generic superclass implementation may use the same empty
throws narrowing receipt when inherited through complete class arguments.
The nearest original provider must remain an accessible instance method and
its substituted parameters, result and method bounds still have to match the
interface contract. Open children and fixed Object children retain their
original method owner, scopes and empty exception declaration without copying
methods or creating a projection. Nonempty, generic or unproven exceptions,
full-type drift and nearer invalid declarations remain rejected. The owned
source and dependency JVM profiles retain 30 values, 128 identities, 51 state
checks and 21 ordered failures, compare all original metadata and inventory,
and reject same-Code type/exception counterexamples and independent javac
incompatible generic or checked throws overrides.

A fully validated closed parameterized result may override the exact original
raw class result, including a raw class argument substituted through a generic
superclass. The common return comparison keeps this rule separate from exact
parameter matching and preserves all arguments when the required result is
parameterized. Open result variables, arrays and parameterized inner class
segments do not use this bounded raw-return rule. Original public/protected
forwarding bridges still require their exact body proof and are reported
separately. The `--parameterized-return` profiles of the inherited-generic
oracle retain the raw superclass argument and `AtomicReference<?>` method
result, compare eight additional values, 32 identities, 16 state checks and
eight failures, and preserve payload mutation, null results and exceptions.
Changing only the superclass argument to `AtomicReference<String>` keeps Code
and runtime descriptors unchanged but fails full return comparison and the
independent javac override check.

An exact complete parameterized member contract may carry fixed declared
exceptions through the same common closed-hierarchy receipt. The target must
retain the identical proven exception set or a proved empty clause, with no
method formals, generic throws or method source projections. All complete
parameter/result types and original bridge bodies remain checked. The erased
bridge retains the interface's original ordered Exceptions attribute, even
when its generic typed target has an empty exception list. The
`--parameterized-throws` source and dependency profiles compare 30 additional
values, 198 identities, 64 state checks and 34 ordered failures. They retain
mutable collection aliases, checked/unchecked exception objects, null behavior,
bridge casts before effects and all original metadata. Same-Code and javac
incompatible parameter/scope or expanded-exception counterexamples remain
rejected; only the two original bridges receive disclosed source projections.

The deferred-plain-callbacks source and `--dependency-interface` JVM profiles
keep the plain interface in source or an unchanged declaration JAR. They check
callback dispatch through child, superclass and interface views, mutable state,
ordered failures and complete generic reflection. Original class and method
inventories remain unchanged except for separately reported runtime helpers;
there are no generic source projections for this profile.

A parameterized interface may inherit a known nonempty interface with a complete plain runtime contract. Its bounded ancestor DAG is consumed by the same full member planner: required implementations, public instance visibility, exact return types and default selection still have to pass. No generic Signature is invented for a plain declaration. Raw generic formals, generic member types, declared throws, fields, cycles and unknown ancestors remain rejected in this profile. The owned source and dependency oracles compare 78 identities and seven failures, retain full generic edge reflection, verify a plain getter and default virtual dispatch, and preserve the original checked generic bridge. A same-erasure method-formal scope mutation is rejected by NeverD and javac. Regenerating the original bridge keeps coverage `partial`.

A fixed source interface may supply a concrete default whose original bridge
casts erased parameters to the complete specialized ancestor contract. The
shared proof requires exact full parameters and result, a closed independent
scope, and the original forwarding instructions. Source and dependency abstract
roots consume the same proof. Both erased and specialized declarations affect
default selection, so an abstract typed shadow or unrelated competing default
cannot be bypassed. Missing bridges, changed receivers or casts, method formals,
throws and conflicting complete interface arguments fail. The default and its
bridge retain their original owner; implementing classes gain no declaration.
Plain intermediate interfaces and concrete consumers without a class Signature
must traverse the same complete ancestor proof; an empty plain interface layer
does not erase the default selection or specialization requirements.

The source-default-parameter-bridges source and `--dependency-root` JVM profiles
check typed, erased and reflective dispatch, mutable order values, subclass
overrides, cast failures before default effects, null behavior and ordered
exception identity. They compare the original class and method inventory and
complete metadata through a plain intermediate interface and concrete receiver,
both without a class Signature, while keeping the dependency JAR unchanged. Two
original Code mutations, a same-Code ancestor argument change, a removed default body, an
abstract typed shadow without a bridge and competing defaults are rejected;
independent javac checks confirm the invalid hierarchies. The profiles run with
JDK 21. Original bridge regeneration is reported as `partial`, with no generic
Signature projection.

An independently scoped source interface without its own formals can supply a zero-argument covariant default with a closed result. The common validator composes its complete generic ancestor arguments and proves the full result relation; an original erased interface bridge also needs its exact forwarding instruction proof. The consumer traverses that fixed source interface and every other generic path, so differently instantiated ancestors cannot reuse an owner-local default proof. The owned JVM oracle keeps the default in its original interface, compares 132 double values by raw bits, 72 identities and six failures, and checks fresh boxing, signed zero, subnormal values, infinities, NaN payloads, subclass dispatch and ordered effects. An incompatible default ancestor argument with unchanged Code and descriptors is rejected by NeverD and javac. Regenerating the original bridge keeps coverage `partial`.

Covariant reference results use the same complete scoped type relation across class edges and the full interface graph. A shared parameter comparison requires full equality after substitution even when two reference parameters both erase to Object; missing consumed metadata and different class-formal scopes fail. Original checked bridges retain argument order and virtual dispatch; abstract interface declarations with an omitted erased entry report the compiler-added entry separately. A generic source interface default additionally requires its exact original erased bridge to pass the instruction proof. A missing bridge cannot invent a default body. The authored indexed default oracle preserves seven classes and thirteen method identities, checks 808 values, 781 identities and seventeen ordered failures through typed, erased, reflective and inherited-default views, and retains complete generic reflection. A sibling class-formal result mutation with unchanged Code and descriptors fails clearly; regeneration of original bridges keeps coverage `partial`.

An original erased covariant bridge inherited from a source superclass retains its complete ancestor contract through the common scoped proof. The consumer composes every superclass argument and compares full parameter/result types, then reverifies the original forwarding body. No Signature or method is added to the consumer. Declaration order cannot change which exact runtime return entry is selected. The `--inherited-bridge` JVM oracle preserves eight classes and twelve methods, compares 742 values, 721 identities and fourteen ordered failures through class, interface, raw and reflection views, and rejects a changed consumer argument with unchanged Code. Original bridge regeneration keeps coverage `partial`.

A source class can retain a covariant generic override of a source or dependency
interface default through the same complete contract proof. Full parameter types
must agree, the typed result must satisfy the complete generic return ancestry,
and the original erased bridge must pass its forwarding-body proof. A subclass
without its own declaration composes every intervening superclass argument and
inherits that class implementation, which keeps precedence over the default.
Original owners and Signatures remain unchanged. Method formals, throws, source
projections, missing bridges and same-erasure type mismatches cannot use this
profile. The generic-default-class-bridges source and `--dependency-interface`
profiles run with Java 25; dependency default bodies remain in the original JAR.

These JVM profiles compare typed, superclass, interface and reflective calls
through a captured anonymous subclass, including listener state, return identity,
original callback casts and ordered target, callback and null failures. A
separate default-only control executes the interface body; inherited class calls
must avoid its failure sentinel. The profiles check the original class and
method inventory, full generic metadata and the absence of copied listener
methods in intermediate classes. Parameter-Signature and original forwarding
body mutations are rejected. Java 25 preserves the original capture before
parent initialization. Original bridge regeneration and the reported
`anonymous-to-named-local` capture and reflection differences keep coverage
`partial`; the covariance proof adds no generic Signature projection.

An inherited nominal reference contract may lack a generic method Signature. Recovery resolves its original runtime descriptor in the same known class scope and compares the complete parameters and result ancestry without adding metadata. Class-declared interfaces use that same bounded closure. The plain-covariant-contracts oracle preserves five input classes, ten methods and three original return bridges, compares eight values, sixteen identities and six ordered failures, and confirms zero parent effects. Complete class/method metadata and runtime inventories match; same-erasure result and superclass argument changes with unchanged Code fail. Regenerated bridge reflection keeps coverage `partial`.

A parent's original covariant bridge can consume a child's original bridge through that same complete scoped contract. Every parameter and nested result argument must match after substitution, and both original forwarding bodies remain independently verified. The `--parent-bridge` oracle preserves six classes, thirteen methods and five original bridges, compares eight values, sixteen identities and six ordered failures, and rejects same-erasure parent/result metadata drift without changing Code. Complete original metadata and inventories remain checked; bridge regeneration keeps coverage `partial`.

An original subclass bridge can override its parent's original erased bridge through the same complete scoped witness. Both forwarding bodies are checked independently, and the typed entries must have equal full parameters and results after superclass substitution. The `--class-bridge-override` oracle keeps eight classes and fourteen methods, compares 742 values, 721 identities and fourteen ordered failures, and rejects parameter, result and superclass argument drift without changing Code. Its typed implementation calls the original superclass method; source arguments use the proven substituted type only when runtime erasure is unchanged. Bridge regeneration keeps coverage `partial`.

A source superclass bridge without a Signature uses its parent's complete typed
contract and its own original forwarding proof. It is not a separate raw source
contract in the child. Every deferred parent bridge must have its own complete
receipt, independently of declaration order; malformed forwarding, missing
entries and competing full generic scopes still fail. The source-class-bridge
oracle covers Object, Serializable and Enum return erasures, retaining nine
original methods, or seven with the root in an unchanged declaration JAR.
The owned enum values remain in an immutable dependency in both profiles.
It compares 56 identities, 28 mutable-state checks and thirteen failures through
typed, parent and root views, checks complete original metadata and inventory,
and rejects three unchanged-Code scope counterexamples plus an independent
javac counterexample. Original bridge regeneration keeps coverage
`partial`.

That source parent can also implement a more distant abstract requirement for
an unchanged child. The original erased entry supplies the runtime descriptor,
its concrete typed target supplies the complete source contract, and the common
ancestor composer compares every parameter and result in the child's scope.
Both original forwarding and nearest-provider ownership remain mandatory;
dependency bridge bodies, missing entries and conflicting scopes cannot use
this receipt. The `--inherited-members` source and dependency-root profiles
retain fifteen or eleven methods and five original bridges. In addition to the
three-erasure probes, they compare 221 identities, 224 state checks and 103
failures, including bridge casts before target effects, unchanged serializable
heap pollution and the caller's typed read cast. Full metadata and inventory
remain unchanged except reported helpers; the child gains no inherited member.
Five unchanged-Code scope counterexamples and the independent javac negative
are rejected. Coverage stays `partial`.

Distinct original final getter bridges may share a JVM name and parameters while retaining different return descriptors. The planner verifies both original getter bodies and full scoped getter covariance before assigning the narrower entry a disclosed Java source name. The inherited final entry remains in its original owner and still dispatches its virtual getter. The final-getter-return oracle keeps six classes and thirteen methods, compares 100 values, 150 identities and eleven ordered failures, and rejects result and superclass scope drift. The source name, binary ABI and bridge reflection differences keep coverage `partial`.

Known generic dependency class ancestors can retain an interface default that implements a more distant abstract self-return contract. The class-parent check uses the same unique maximal interface provider as direct default selection and requires the existing complete covariant witness. Source class prefixes compose every original interface argument before consuming that provider; dependency bodies stay unchanged. The `--class-ancestor` external-default oracle retains three input classes and five methods, compares 43 identities and sixteen failures, and verifies the original dependency bridge, redundant generic roots, subclass dispatch and effects before exceptions. Coverage is `recovered`.

Default selection includes interface branches introduced by every original superclass. Their complete interface closure is composed into the consumer's scope before choosing the unique most specific provider; repeated interfaces require equal full arguments. The `--class-interface-branches` oracle preserves five classes and five methods, compares 43 identities and sixteen failures with unchanged dependency code, and rejects superclass interface argument drift with unchanged Code. Coverage is `recovered`.

A source override of an interface-only dependency default can retain the known class ancestor when the existing complete plain contract comparison proves its parameters, result, visibility and instance kind. Generic scopes and competing overloads still require their own proofs. The combined `--class-interface-branches --plain-default-override` oracle preserves five classes and six methods, compares 43 identities and sixteen failures, and retains the original superclass default invocation and dependency effects. Coverage is `recovered`.

A fixed generic source class prefix can override an inherited dependency default through its original checked bridge. Class-declared interface roots enter the complete member graph before override selection; the typed implementation needs full parameter/result equality, and the original erased entry must pass the forwarding proof. The fixed-scope-default-override oracle preserves two source classes, five methods and one original bridge, compares nine values, 26 identities and twelve ordered failures, and verifies that dependency default effects remain unchanged. Same-erasure superclass and interface metadata drift with unchanged Code fails. Bridge regeneration keeps coverage `partial`.

A raw unary superclass formal may gain a reported source argument when that formal affects no original field or member contract and passes unchanged only through known Object-bounded marker ancestry. The consumer's complete interface graph must supply one exact argument on every relevant path; member-dependent formals, non-Object bounds and conflicting arguments fail. Raw carrier interface projections keep the superclass raw. The raw-marker-superclasses source/dependency oracles retain two classes/five methods and one class/three methods, compare twelve values, fourteen identities and six ordered failures, and check full class/method metadata plus complete runtime inventories. Input Code and descriptors remain unchanged; only the disclosed superclass Signature changes, keeping coverage `partial`.

A named or anonymous captured member can inherit class bodies and the unique interface default through the same complete source scope. Only an already validated `static-inner-type-variable` or `anonymous-member-type-variable` projection to `Object` is eligible; every superclass and interface argument is composed and compared in full. Bounds, raw edges, nearer inaccessible or abstract slots and competing defaults still fail. The captured-sdk-inheritance oracle retains three classes and nine methods, compares 103 values, 44 identities and 21 ordered failures, and checks mutable outer captures, inherited array method formals and exceptions before effects. It verifies the exact original runtime inventory plus reported helpers and rejects same-erasure bound changes with unchanged Code. Static member, capture-field and generic reflection differences keep coverage `partial`; preserved constructor order requires Java 25.

The anonymous inherited-contract oracle retains four source classes and thirteen methods, or two source classes and six methods with unchanged owned parent declarations. Both profiles compare 91 values, 105 identities and 29 ordered failures, including parent-constructor virtual dispatch before owner initialization, subsequent outer mutations, listener and executor failures, null receivers and the original interface default. The independently compiled harness is reused unchanged for rebuilt execution. Complete metadata and inventories are checked against only the reported member binding, capture-field flag and two Object scope projections. Same-erasure outer-bound and inherited-result changes with unchanged Code are rejected.

An unchanged inherited class method may declare fixed exceptions when the shared signature validator has proved both complete exception hierarchies and the same exception set. The original declaration order remains unchanged; generic exception Signatures, different sets, unknown classes, duplicate exceptions and cyclic hierarchies fail. The fixed-throws source/dependency profiles retain four classes and fifteen methods or two classes and six methods, compare 178 values, 241 identities and 53 ordered failures, and preserve IO, reflection and timeout exception identities, wide duration arguments, effects before null failures and reflection order. An Exceptions-only interface mutation with unchanged Code, runtime descriptors and Signatures is rejected before output.

Captured named siblings may inherit unchanged methods through a two-segment
`Owner<T>.Base` superclass only when the common validator proves both original
constructor captures and the same ordered outer-owned formals. The
`test_mobile_android_captured_sibling_ancestors.py` oracle compares seven direct,
interface, reflection and MethodHandle routes, mutable outer aliases, arrays,
method-owned shadowing formals and ordered failure identities. Source and
immutable interface JAR profiles retain five classes/fourteen methods and three
classes/ten methods. They compare 70 values, 133 identities, 147 states and 35
failures, verify original inventories and disclosed static/capture/Signature
metadata changes, and reject unchanged-Code owner bound, owner substitution and
method result drift. Original inherited entries stay in the parent; coverage
remains `partial` and constructor order requires Java 25.

The `--inherited-default` profile also checks a unique interface default through
both captured sibling classes. Source and interface JAR profiles retain five
classes/fifteen methods and three classes/ten methods. The unchanged original
harness remains active, and another seven-route oracle compares 42 values,
63 identities, 56 states and 28 failures for callbacks, mutable outer aliases,
null receivers/callbacks and effects before inherited or callback exceptions.
Default selection reuses the exact captured edge receipt while still rejecting
competing providers, abstract shadows, nearer class slots and inconsistent
interface arguments. No forwarding default method is added to either member.

A final guarded reference bridge can also override a known public plain superclass contract. Full generic class edges and class-declared interface roots retain exact scoped arguments; incompatible repeated interface arguments and final or static slot changes fail. The original guard, false result and virtual call remain unchanged. The `--class-ancestor` and `--dependency-root` oracles retain four source classes/seven methods and three source classes/five methods respectively, compare 22 values, seventeen identities and six ordered failures, and verify zero ancestor effects through superclass views. The report discloses `guarded-class-forwarding-bridge`, its original class witness and reflection flag differences as `partial` coverage.

An unchanged original guarded bridge can also remain inherited through a generic source class prefix. The nearest class declaration is checked with the same original guard, checked argument and virtual target proof; its public runtime contract still has to match exactly. No bridge is added to the child. The `--inherited-guarded` source and dependency profiles retain five classes/eight methods and four classes/six methods respectively, verify 22 values, seventeen identities and six ordered failures, compare complete original class/method metadata and reject changed guard behavior. Coverage remains `partial` for the original ancestor bridge flag.

Named captured members can retain the existing exact final integer getter bridge and guarded boolean/integer query bridges after the common planner proves the outer field and constructor parameter. The original plain interface slots, complete scoped metadata, type check or predicate, fallback value and virtual typed call remain checked. The captured scalar JVM profiles preserve original inventories and full metadata except reported static-member, capture-field and bridge flags; independently compiled subclasses stay outside the DEX and verify original virtual dispatch. The getter checks twelve values and four failures, the boolean guard twenty values, sixteen identities and five failures, and the integer guard 35 values, 31 identities and five failures. Each also checks four outer identities and rejects added forwarding effects. Preserved pre-parent captures require Java 25 and coverage remains `partial`.

The typed target of an original guarded bridge may itself be an original scalar superclass adapter. Its complete three-instruction body must pass the same fully scoped superclass call proof, invoke the immediate superclass with the unchanged receiver and widened argument, and return the original boolean or integer result. The superclass declaration may lack a generic method Signature; no metadata is invented. The superclass-scalar-adapters source/dependency oracles preserve four classes/seven methods and three classes/five methods respectively, compare 37 values, thirty identities and nine ordered failures, and check virtual overrides, superclass effects, full metadata and complete inventories. Added forwarding effects fail. Both bridge flag differences are reported as `partial` coverage.

An original checked reference superclass adapter retains its widened call, original result cast and complete scoped return type. Its guarded Object entry preserves the null path and virtual typed dispatch. An erased return bridge is regenerated only when its original guard and call body is identical to that retained entry. The checked-reference-super-adapters source/dependency oracles retain five classes/nine methods and four classes/seven methods, compare fourteen values, 69 identities and twelve ordered failures, and verify complete runtime inventories, unchanged generic reflection and original effects before cast or callee failure. Same-erasure return and superclass argument changes with unchanged Code fail. Bridge/final flag and forwarding frame differences keep coverage `partial`.

A two-argument guarded default-result bridge retains the original incoming default on its negative branch and checks that default only after the original key guard succeeds. Both typed and erased guard bodies remain explicit methods, preserving virtual overrides and effects before failures. The complete known superclass/interface graph must select one inherited default or nearest concrete class declaration with identical full default/result types. Source superclass and interface arguments are explicitly projected to raw forms to keep the original erased entry expressible; all member proofs still consume saved original contracts. The guarded-default-super-adapters source/dependency oracles retain five classes/nine methods and four classes/seven methods, compare seventeen values, 73 identities and fifteen ordered failures, check unchanged method metadata except reported bridge/synthetic flags, and reject same-erasure metadata drift with unchanged Code. Raw ancestry metadata and bridge flags keep coverage `partial`.

Distinct same-owner return entries can preserve a final broad call to an original private producer while a narrow entry stays virtual. Recovery proves both complete bodies, the exact same original private target, full narrow/producer result equality and full broad covariance before assigning one disclosed source name to the narrow interface/implementation family and its known callers. The broad final entry keeps its name and private dispatch. An erased bridge into a renamed target retains its original forwarding body explicitly. The private-producer-return-slots JVM oracle preserves eight classes and fifteen methods, compares 144 values, 132 identities and six ordered failures, checks child/grandchild override effects separately from private production, compares full class/method metadata except exact reported names, and rejects same-erasure metadata drift with unchanged Code. Runtime names, binary ABI and name reflection keep coverage `partial`.

An original nonfinal bridge may forward a superclass result with broad `Object` metadata while a final getter exposes the complete class-formal result. Recovery checks both unchanged three-instruction bodies and compares the wrapper's full result with the substituted original dependency method before projecting only that bridge's source Signature. Its original metadata, body and runtime descriptor remain in the report. The superclass-getter-result oracle preserves three classes and nine methods, compares 35 values, 36 identities and six failures, and rejects changed metadata, witness or superclass arguments with unchanged Code. The source Signature and bridge reflection changes keep coverage `partial`.

The same original forwarding-body proof also supports signature-free integer super getters and their final virtual wrappers. The `--integer-getter` oracle preserves three classes and fourteen methods, compares 70 values, 36 identities and thirteen failures, and exercises superclass dispatch, virtual overrides, integer limits and exceptions before effects. Integer metadata stays unchanged; bridge flag changes and the separately proven reference result projection keep coverage `partial`.

Several direct external roots may coexist when every root has that complete chain proof and each relevant runtime slot has one unambiguous default provider. A root with no such slot is disjoint; repeated paths to the same original declaration remain valid after the common generic-argument checks. An unrelated second default owner, a competing abstract slot or an unknown graph fails. The independently authored iterator/entry implements both SDK interfaces, preserves its original return bridge and inherits Iterator behavior without copying dependency code. Recompilation retains all ten input descriptors and generic reflection; the JVM oracle compares 24 identities and eight ordered failures through typed and erased iterator, entry and default-consumer views. Mutable state, alias identity, writes after checks and effects before thrown exceptions match. Swapped generic return arguments with unchanged Code and descriptors fail both NeverD and javac. Regenerating the one original bridge keeps coverage `partial`.

An input class with a complete known superclass chain to Object may satisfy a plain abstract interface slot through a known external Object declaration. The complete runtime parameters and result must agree, both methods must lack a generic Signature or declared exceptions, and the root implementation must be public, concrete, nonstatic and nonfinal. Object must retain its plain root class role. A same-slot input declaration cannot be bypassed; unrelated overloads stay separate. No body, declaration or bridge is fabricated. The independently authored generic interface redeclares equals, hashCode and toString, and the input class retains a distinct String overload. The JVM oracle recompiles all five input descriptors, checks 35 identities and eight ordered failures, confirms that root methods still dispatch to Object during input failures, and preserves overload effects, array identity, class/method scopes and generic reflection. The `--class-ancestor` and `--dependency-root` profiles retain two generic superclass edges, including an unchanged dependency root, and verify the same 35 identities and eight failures across three source classes/seven methods and two source classes/six methods respectively. Unknown or cyclic class edges, raw generic arguments, nearer abstract slots, source-owned Object declarations, inaccessible or static providers, declared exceptions, final slots and incompatible input declarations fail. Coverage is `recovered`.

A public concrete class method inherited through a known bounded superclass chain may supply an abstract interface contract without a new input declaration. Each invariant generic superclass edge composes the common validator’s exact declaring scopes. The nearest erased slot must retain every original parameter and result descriptor, visibility and instance kind; an inaccessible, abstract, static, bridge or incompatible declaration cannot be bypassed. Its complete substituted parameters and return must match the interface contract, including nested class arguments and method-formal alpha positions. Missing consumed Signatures, source projections and declared exceptions require separate proofs. Source keeps the original ancestor method body and does not add a bridge merely because a child’s formal has a narrower bound. The independently authored dependency interface and six input classes span two generic superclass edges, an open child and a Tag-bounded child. All eight input descriptors recompile without bridges; 21 identities and eight ordered failures verify root/middle/interface views, mutable pair returns, generic reflection, effects before exceptions and an erased wrong-type call accepted by the original Object-based method before a later typed field cast fails. Generated private exception helpers add no public API. Reordered same-erasure provider return arguments with unchanged Code and descriptors are rejected by NeverD and javac. Coverage is `recovered`.

The same complete inherited-class proof retains class-method precedence over an external interface default. The public concrete ancestor must own the unchanged erased slot, and full generic parameters and return must agree after superclass composition. Unproven class competitors still cannot be bypassed to select a default. The independent API-24 oracle places an effectful, throwing default and its tracing field in an unchanged dependency JAR. It recovers the six input classes and eight descriptors, checks 21 identities and eight ordered failures, preserves generic API reflection and inherited method ownership, and confirms zero default effects through every class/interface view. The bounded raw-call behavior stays unchanged without a bridge; coverage is `recovered`.

An inherited `(Object, Object) -> boolean` class implementation can retain two Object-bounded class-formal parameters when supplying a plain external interface default of the same erased slot. The common validator proves this narrow contract without changing generic metadata: a top-level input parent extends Object without interfaces, the direct external interface is known, and both declarations have two exact Object bounds and matching invariant superclass/interface arguments. Only public concrete instance methods without method formals, bridges or declared exceptions qualify. A known descendant declaring another two-argument member of that name requires its own override proof. The original parameters, class formals, runtime descriptors and bodies remain unchanged. The independently authored fixture inserts only a method Signature into a valid classfile and recompiles all ten descriptors. Twenty-nine identities and twelve ordered failures verify generic reflection, inherited class ownership, subclass dispatch, zero bridges and zero default effects. Independently compiling the original class-formal source also preserves the same JVM behavior. Unproven method-formal scopes, access, bounds, exceptions and descendant overrides remain rejected. Coverage is `recovered`.

A direct public input subclass may override an Object-bounded class-formal parameter with its exact plain Object erasure while retaining the complete substituted return type. The common validator records the exact target/ancestor pair only when invariant superclass arguments bind to the child’s own Object-only formals, both declarations retain identical descriptors and instance kind, and neither method has method formals, source projections, bridges or declared exceptions. The parent is a top-level input class extending Object without interfaces. The child parameter types must all be plain Object; nested parameterized arguments and arrays do not use this proof. Returns still require exact full scoped substitution, so changing a result to a sibling formal or dropping its consumed Signature fails. Existing final and visibility checks remain authoritative. The same JVM oracle checks boolean and type-variable-return overrides through superclass and interface views, effects before failures, and unchanged generic reflection. No body, bridge or metadata is rewritten.

An original public final `()I` bridge can retain its complete forwarding body when implementing a known external plain interface slot. Its three original instructions must invoke a distinct public concrete same-class `()I` getter virtually on the unchanged receiver, move the integer result and return that exact register. Independent source scope, an Object superclass, original final visibility and the absence of generic method metadata, declared exceptions, handlers, branches or extra effects are required. The original declaration, final flag, runtime descriptor and virtual getter dispatch stay in Java source. `plain-interface-forwarding-bridge` records the target and the unavoidable bridge/synthetic reflection flag difference; coverage remains `partial`. The independent JVM oracle mutates only ACC_BRIDGE on an authored classfile with unchanged Code and descriptors, keeps its interface in an unchanged dependency JAR, recompiles all five input descriptors across the parent and authored input subclass, and verifies 12 values and four ordered failures through class/interface/subclass views. The input subclass overrides the getter and redundantly implements the exact generic interface; the final bridge remains declared only by the parent. A bridge with extra field effects is rejected rather than approximated.

An original public final no-argument reference getter bridge can retain its full generic Signature and three-instruction virtual forwarding body. The complete inherited result, bridge result and distinct same-class public getter result must agree after the common validator resolves every variable to its exact declaration scope, including nested generic results. Only the unchanged receiver, immediate reference result and return are allowed; method formals, source projections, missing consumed Signatures, declared exceptions and extra effects fail. The class must retain an independent Object-based source scope. `generic-reference-interface-forwarding-bridge` records the original target, body and descriptor while reporting the bridge/synthetic reflection flag difference. The independent flag-only JVM oracle retains all five input methods across the parent and authored input subclass, checks ten values, fifteen identities and four ordered failures, verifies mutable collection identity and inherited subclass getter dispatch, and preserves the complete method/interface/superclass generic reflection. An authored bridge with extra effects is rejected; C++ counterexamples also reject same-erasure variable drift and missing or method-local target scopes. Coverage stays `partial` without a generic metadata projection.

An original public final `(Object) -> boolean` bridge can likewise retain a proven type-test forwarding body. The original instructions must test the incoming argument against a known reference type, return false for an incompatible value, and otherwise cast the same argument to that exact type and virtually invoke the same-name public concrete typed overload on the unchanged receiver. Both conditional layouts and an optional argument copy are checked; every instruction must belong to these two paths. The target retains its full generic parameter Signature and complete original effects and exceptions. Method formals, declared exceptions, handlers, extra effects, changed branch results or unproven casts and dispatch fail explicitly. `guarded-interface-forwarding-bridge` reports the unchanged descriptor and body, together with the bridge/synthetic reflection flag difference; coverage remains `partial`. The independent flag-only classfile oracle retains two input classes and all four descriptors, verifies 20 values, 16 identities and five ordered failures, checks null and incompatible arguments, mutable target effects, typed subclass overrides and unchanged generic reflection, and rejects an authored true-return counterexample.

The same original type-test proof supports `(Object) -> int` bridges. Their incompatible-input path returns its exact original 32-bit constant, including `-1`; the compatible path retains the typed virtual call and its integer result. No exception, cast or target effect is added to the rejected path. The independent `--guarded-integer` oracle checks 35 values, 31 identities and five ordered failures, preserves all four runtime descriptors and complete generic reflection, exercises integer extremes and a typed subclass override, and rejects an extra rejected-path effect.


The same original-body guard proof can retain a known public static `(Object) -> boolean` predicate followed immediately by its scalar result and branch. The exact unchanged incoming argument must supply that call; its declaration cannot introduce a generic Signature or declared exceptions. No predicate meaning or subtype implication is inferred. The call, its side effects and failures, the false branch, the original checked cast and the typed virtual target remain in place. `guard_predicate` names the exact retained declaration. The independently authored predicate can record arguments, throw before the target, reject compatible values or force an incompatible/null value down the checked path. Its JVM oracle retains three input classes and six methods, compares 37 values, 16 identities and five ordered failures, verifies effects before both predicate and target failures, checks that the original cast still fails for an incompatible value accepted by the predicate, and preserves typed generic reflection and original final visibility. Unknown predicates, changed dispatch/arguments/results and branch drift fail explicitly; the bridge flag difference remains reported with `partial` coverage.

A checked value-carrier bridge preserves the original two virtual unboxing calls, the `long,long -> long` instance target and the static boxing factory. The common validator requires a final source class, a complete direct Object-bounded interface contract with one to four arguments, known public carrier declarations and the exact original checked call sequence. It verifies wide register pairs and rejects clobbered values or reordered arguments. It does not infer carrier layout or replace any carrier implementation. `checked-value-carrier-interface` records the original interface Signature verbatim and projects its arguments to raw source; `value-carrier-interface-bridge` retains the complete original body, runtime descriptors and exact boxing/unboxing targets while reporting changed bridge/synthetic flags. Dependent source bounds are rechecked. The independently authored D8/JVM oracle retains five classes and ten methods, compares 48 values, 13 identities and eleven ordered failures, verifies wide edge values, factory allocation, callee effects and unchanged typed primitive calls, and rejects swapped carrier metadata with identical Code and descriptors. A mixed variant leaves checked reference arguments unchanged, unboxes only the original wide-carrier inputs and forwards an exact plain reference result without a boxing factory. Its separate JVM oracle compares sixteen wide values, sixty identities and nine ordered failures; nullable reference arguments retain their values, live references cannot be clobbered by wide register pairs, and no extra unboxing or boxing call is introduced. A parameterized reference result is accepted only when the common-validator-resolved target Signature agrees with every original parameter and the complete implemented-interface result, including nested arguments, variance and declaration scope. Method formals, projected target metadata, missing target Signatures and same-erasure argument drift remain unproven. The separate generic-result oracle retains six classes and eleven methods, compares 32 values, 29 identities and ten ordered failures, checks unchanged target return reflection, and rejects changing `Box<CarrierA>` to `Box<CarrierB>` with identical Code and runtime descriptors. The same carrier proof reuses the authoritative Integer/Number and Boolean conversion table for narrow primitive inputs. A Number cast is accepted for an Integer contract only with the exact known virtual `Number.intValue()` declaration and boxed hierarchy; other Number implementations retain their original dispatch. A reference-returning target keeps any original boxing call inside its complete body. The integer-reference JVM oracle retains three classes and five methods, compares forty values, eight identities and eight ordered failures, verifies small-value Integer.valueOf cache identity, integer edge values, non-Integer numbers and an authored effectful Number override, and rejects Boolean interface metadata with unchanged Code and descriptors. Coverage stays `partial`.

Primitive array results share the complete generic result and original forwarding proof. The independent `--primitive-array-result` oracle retains three classes and five methods, checks 453 values, 134 identities and eight ordered failures, and compares two ordered Number conversions followed by the original `int,int -> int[]` target. It checks cached arrays, null, caller mutation, integer extremes, exact exception identity and unchanged target and field reflection. Changing only the original interface result from `int[]` to `long[]` is rejected despite unchanged Code and runtime descriptors; no array allocation, copy, boxed-type cast or boxing call is introduced.

Reference-backed carrier inputs reuse the same complete original conversion proof. A cast to the declared carrier may be followed by one known public virtual zero-argument method returning a known plain reference. Its complete declared return, rather than the wrapper type, must equal the target parameter; the separately validated parameterized-carrier profile also admits resolved owner formals when the conversion return is a literal reference, while generic conversion results and invented target arguments remain unproven. The independent `--reference-carrier` oracle retains five classes and eight methods, checks 87 values, 58 identities and six ordered failures, and verifies nullable payload identity, mutation, converter subclass overrides, original cached Boolean results and exact exception identity. Target and field reflection remain unchanged; boxing stays inside the original typed body. Changing the original generic result with unchanged Code and descriptors is rejected.

An array-backed carrier conversion may return a closed array with known component types. The original void target must consume that exact array type, then the bridge must read the known `Unit.INSTANCE` field and return its unchanged value. The report records `result_field`; no field read occurs before target success. The independent `--array-unit` oracle retains the original bodies and descriptors, compares 159 values, 75 identities and eight ordered failures, and observes first singleton initialization after a successful target while failed conversions and targets leave it uninitialized. It checks nullable and empty arrays, raw float bits including NaN payloads and signed zero, original array mutation, converter subclass overrides, exact exceptions and unchanged target, converter and field reflection. A changed generic result with identical Code is rejected.

The `--parameterized-reference --open-reference` extension of the reference
and array-Unit carrier profiles uses `Carrier<? extends S>` with the receiver's
original class formal. The immutable harness checks that exact original upper
bound before recovery. The declaration-only conversion still returns a literal
reference independent of `S`; raw source projection and bridge flags remain
disclosed. Full class, field and method metadata are compared outside those two
changes. A same-Code generic conversion Signature is rejected, as is an
incompatible interface result. Input, harness and SDK bytes stay unchanged.
Native tests additionally cover three arguments, live-value clobbers, changed
argument order, source/SDK carrier declarations and reversed class/method order.
The reference profile retains five classes/eight methods and compares 87 values,
58 identities and six ordered failures. The array-Unit profile retains five
classes/nine methods and compares 159 values, 75 identities and eight failures,
including mutable float bits and singleton initialization after target success.

Covariant generic getters may override a method declared beyond the direct source superclass. The common validator composes each original invariant superclass edge, preserving declaration scopes and argument permutations, before proving the full parameterized result subtype. The inheritance planner validates every relevant ancestor slot and the original self-forwarding bridge. An intermediate class need not redeclare the method. The independent three-edge javac/D8/JVM oracle checks 24 identities, five failures, ancestor-view dispatch and generic reflection. A Signature-only permutation with identical erasures and unchanged Code is rejected by both NeverD and javac.

A source generic parent may combine parameterized interfaces with a known nongeneric superclass chain. The common planner validates each unchanged input generic interface separately, while the plain ancestor proof still checks every non-generic declaration, implementation and child override. Unknown declarations, missing concrete providers, cycles, static/final competitors and full type drift remain errors. The independent mixed-ancestor oracle checks six classes, thirteen original methods, 34 identities and eight failures; the prior package-visible generic-chain oracle also retains its 46 identities and seven failures.

A concrete package-visible class method that is inaccessible from a child in another package is not a child contract. One visibility predicate owns this decision for both generic and plain ancestor proofs, before final and static/instance competitor checks. The original declaring class, Signature and body remain unchanged; an independently declared same-name child method does not gain a bridge or override. Same-package members and public/protected contracts still receive the full checks. An inaccessible abstract member cannot use this skip and still requires a supported original implementation proof. The independent source and SDK profiles retain four classes/thirteen methods and two classes/six methods respectively. They compare all original class and method metadata, 32 values, 32 identities and eight ordered failures, including parent state changes before a bridge cast failure, subclass dispatch and independent hidden final/static calls. Three Code-preserving counterexamples make a final or static method visible or change a class formal; NeverD rejects them, and javac independently rejects the visible ancestor conflict. Bridge coverage remains `partial`.


Reference-backed result boxes use a bounded original-body certificate: a known final input carrier, a public final static factory that only allocates it, and a private constructor that calls Object then stores the unchanged reference in a private final field. The full fixed field Signature must equal the typed target result; the original bridge's move, factory argument and returned box are checked separately. Open representation variables, added factory/constructor instructions, changed field arguments or result registers do not use this proof. Carrier type arguments absent from the fixed backing field remain original metadata and are disclosed separately with the raw source interface; no argument mapping is invented. Reports name the original factory, representation field and complete fixed Signature. The independently authored javac/D8/JVM oracle checks 15 values, 44 identities and four failures, fresh boxes, nullable and mutable payloads, cold initialization only after target success, unchanged backing reflection, and a Signature-only backing argument conflict rejected by NeverD and javac.

Custom integer carrier conversions use the exact known public virtual zero-argument declaration and original `int` or `long` result width. Their void targets may use the same original post-call Unit field read. The independent `--scalar-unit` oracle checks 104 values, 23 identities and eight ordered failures, including signed integer extremes, mutable wrapper values, converter subclass overrides, cached singleton identity and cold initialization after target success. Standard Integer and Boolean Unit conversions retain their existing proof owner; ordinary Unit forwarding and scalar carrier forwarding cannot acquire competing source plans. Wide pair order and width drift have separate direct and linked C++ coverage.





Source-owned public interface defaults use the same most-specific declaration selection and complete generic contract checks. Their original bodies remain in the declaring interface; implementations do not acquire copied defaults. Static interface bodies retain their original dispatch and descriptors. Private interface methods remain private, are excluded from inherited slots and retain direct calls. The ordinary body emitter verifies all original instructions and exception paths. Interface body recovery requires Java 9 for a disclosed private exception helper, and the report records `minimum_java_release=9`. Interface class initializers still fail unless ordered field initializers have a separate proof. The independently authored D8/JVM oracle retains four classes and six methods, compares three values, 27 identities and six ordered failures, verifies public overriding, private dispatch against a same-named public class method, static calls and unchanged generic reflection, and rejects a default Signature with mismatched runtime erasure. Ambiguous defaults and more-specific abstract slots remain rejected.

A generic source superclass may retain known nongeneric interface roots after each complete member contract is proved. The shared bounded interface DAG selects unique defaults and compares full method signatures with alpha-equivalent Object-bounded method formals; scoped class variables, nested arguments and variance remain distinct. Direct parent providers must be public instance declarations with the same complete contract, and an abstract provider requires an abstract parent. Interface-only child overrides need that exact runtime slot and full signature. Missing implementations, raw generic roots, competing defaults, added bridge requirements, changed visibility and same-erasure signature drift remain unproven. The independent JVM oracle retains seven classes and ten methods, compares 27 identities and six ordered failures, checks final parent getters, default/class dispatch and unchanged reflection, and rejects a method-formal metadata change with identical Code and descriptors.

A covariant external default returning its own unary Object-bounded interface may dominate an ancestor self-return declaration after the common validator proves the complete generic subtype relation. The proof composes exact invariant interface edges with declaration-scoped substitutions and uses the existing bound/type comparison; equal runtime erasures do not suffice. A public synthetic bridge declared in the same unchanged dependency is retained only as a return slot of that proven ancestor contract. The default selector still requires one most specific declaration owner and rejects abstract or unrelated competing slots. A more specific concrete dependency default can satisfy its abstract ancestor contract through that same proof; the ancestor need not declare a default body of its own. No dependency method or bridge body is reconstructed. The authored dependency oracle retains its original covariant bridge, compares 43 identities and sixteen ordered failures across child/class/interface/raw views, checks target effects and unchanged generic reflection, and rejects a dependency return Signature mutation with unchanged Code, descriptors and input DEX.

An unchanged external default can also be inherited through a source parent that redundantly implements the same complete direct interface roots. The class-edge substitution uses the same helper as inherited concrete member proofs. Each parent root must resolve to the identical child-scoped Signature; no new provider, raw edge or same-erasure formal drift is admitted. Every class on the path remains source-owned and cannot declare a competing erased or specialized slot. The default stays in the unchanged dependency with no fabricated source method or bridge. Where class ancestors have only empty marker interfaces, the original nominal default proof remains sufficient even when the common validator has projected an unrelated captured inner type variable; that existing projection remains reported. The authored default-chain oracle includes both parent and child inputs, checks 43 identities and sixteen ordered failures, verifies generic default return identity and dependency effects, and preserves superclass/interface reflection.

The same generic reference forwarding proof permits one original `int` index parameter. Both complete method Signatures must retain exactly `(int)` and the same scoped result, including a direct class formal or parameterized reference. The three instructions must virtually call the distinct same-class target with the unchanged receiver and original index, immediately move the reference result and return it. Other parameter profiles, reordered or replaced registers, missing consumed Signatures and changed result scopes remain unproven. The independently authored flag-only JVM oracle includes a redundant generic input subclass, retains all five input descriptors, checks mutable collection removal, index boundaries, return identity, inherited getter dispatch and exact target exception order. `generic-reference-interface-forwarding-bridge` continues to report the original body and the unavoidable bridge/synthetic flag difference; coverage remains `partial`.

A source subclass can inherit those proven original final integer and generic reference getter bridges. The member check reproves the parent body through the same authoritative helper, independently of class iteration order, then composes the full generic superclass edge and compares complete scoped parameters and result. The child gains no declaration or bridge. Inaccessible or abstract parent members, changed bodies, mismatched class formals and a child declaration attempting to override the final slot remain rejected. Both authored JVM getter oracles include the subclass as recovered input and check inherited virtual dispatch, unchanged generic superclass/interface/getter reflection and the absence of an added child bridge.

A final top-level input class can retain a no-argument public method whose unary generic result contains invariant class formals while a complete inherited abstract contract requires nested upper wildcards. The common validator composes bounded known superclass and interface edges using the same scoped contract specialization, including source declarations, dependency declarations and known plain marker edges. The full inherited result must use the same external unary result interface. The input interface argument and nested result must name the same class and the same resolved input formals in every position; only invariant-to-upper-wildcard differences qualify. Method formals, lower wildcards, different scopes, declared exceptions, inaccessible targets and unknown contracts remain rejected. `inherited-nested-return-variance` projects only those result argument variances and records the exact inherited declaration, original Signature and source Signature. Runtime descriptors, original bodies, fields and class interfaces remain unchanged; return generic reflection changes and coverage stays `partial`. An independent Signature-only classfile oracle retains two input classes and three methods, checks ten values, fourteen identities and four ordered failures, verifies shared mutable iterator identity and unchanged field/interface reflection, and rejects a same-erasure formal counterexample. The `--class-ancestor` and `--dependency-root` profiles retain two generic superclass edges and a source or unchanged dependency interface branch. They preserve five source classes/six methods and three source classes/four methods respectively, with the same ten values, fourteen identities and four failures. javac independently rejects the original incompatible nested return declaration and compiles the projected source without adding bridges.

The nested return proof also permits an exact known nominal class argument,
including its complete generic shape, under the same inherited upper bound.
An invariant plain Object argument may become the inherited unbounded
wildcard; `? extends Object` shares that canonical bound. Other nominal leaves,
lower bounds and full type arguments must still agree. The iterator JVM oracle
checks 132 values, 516 identities and 67 failures, including shared iterator
consumption and removal. It retains class/field metadata and every body, checks
the exact reported return Signature, and rejects complete key argument drift,
non-Object wildcard widening and lower bounds.

A final input class may retain an original public synthetic bridge that unconditionally throws the same UnsupportedOperationException as its typed overload. The shared proof requires the exact four-instruction allocation, string constant, constructor and throw sequence in both methods, identical local frames, no handlers or declared exceptions, and no incoming-argument reads. The exact external interface contract and complete substituted parameters and result must agree; indexed contracts allow at most two parameters. A typed method without Signature metadata must have plain complete runtime types equal to that substitution. `terminal-throw-raw-interface` records the raw Java interface projection needed to keep both original overloads; the common plan retains the original resolved parameterized interface separately so the bridge planner still checks every complete inherited contract. `terminal-throw-interface-bridge` retains the original erased body without introducing a checked cast and reports bridge/synthetic reflection differences. All original runtime descriptors and bodies remain, while interface and separately projected nested return metadata differ; coverage stays `partial`. The independent oracle preserves two input classes and five methods, checks seven values, eight identities and twelve ordered failures, and verifies that compatible, incompatible and null arguments all retain the original exception and message. It confirms unchanged typed parameter and field reflection and rejects same-erasure formal drift despite unchanged Code. Independent javap verifies allocation before string resolution in both original and rebuilt methods.

The indexed terminal-throw oracle covers `add`, `insert`, `replace`, `remove` and two original default overrides with unchanged runtime descriptors. Four erased methods retain their complete original bodies; the result-only `remove(int)` and `first()` bridges are regenerated by javac because Java cannot declare two overloads differing only in return type. `terminal-throw-result-bridge` reports the additional forwarding stack frame, while the original typed method retains exception allocation, message and throw. The independent JVM checks 108 ordered failures, 108 fresh exceptions and 108 stack frame cases, including incompatible references, null, and extreme indices. It rejects a different original interface type argument despite unchanged Code. The owned contract also redeclares both defaults from a parameterized parent interface, proving that repeated ancestor checks keep the original bridge witness after a result-only source overload is suppressed.

A terminal-throw owner may also inherit through a bounded chain of known ordinary classes with no interfaces and no class, field or method Signature metadata. The shared proof rejects cycles, missing ancestors, final parents and unsupported lexical scopes before changing the owner's interface edge. The plain-superclass JVM profile retains four classes and eighteen methods, verifies the original two-level hierarchy, constructor state and inherited field mutations, and checks the same 108 failures, fresh exceptions and stack frames. All superclass metadata remains unchanged.

The same terminal-throw proof admits a named captured member only after the shared constructor planner proves the original outer capture in every constructor. Complete original interface contracts remain authoritative across that source binding. The captured-member JVM oracle retains three classes and sixteen methods, checks the same 108 failures and stack frames plus original outer identity, and verifies full class, field and method metadata except the reported raw interface, static member, capture-field and bridge flags. Java 25 preserves the original capture before parent initialization; coverage remains `partial`.


The exact unbounded wildcard spellings `?` and `? extends Object` share one canonical comparison rule in the generic validator and inherited-member substitution. The original argument variance, declaration syntax and Signature metadata remain unchanged. Only the plain `java.lang.Object` class node qualifies; a type variable with Object erasure, a specific upper bound, an array, a lower bound or an invariant Object argument remains distinct. A formal replaced by a proven literal Object argument is normalized after full substitution; a still-open Object-bounded formal stays distinct. Both override directions retain identical runtime descriptors without a bridge or source projection. The same owned JVM fixture implements a `Collection<?>` contract with `Collection<? extends Object>`, verifies wildcard reflection equality, retains collection identities and effects before failures, and recompiles with its original generic metadata. A second input binds its interface formal to literal Object and uses an unbounded source wildcard, covering normalization after substitution.

Redundant direct external interface roots can keep a unique more-specific default. Each root still needs a bounded known linear dependency chain, unchanged complete generic arguments and exact runtime slot. The nearest concrete provider from every root must either name the same declaration or be dominated by one unique provider whose declaring interface inherits every other provider. Root order does not select the method. Unrelated defaults, an abstract competitor, unknown roles or ancestors, cycles, differing runtime results and competing class slots remain rejected. The external-default-chain oracle now declares both the generic leaf and its root directly, preserves both original generic edges, and retains the middle default’s effects while confirming zero root-default effects through every interface view. No dependency body or input bridge is copied.

Source generic superclass chains also retain their original abstract and concrete declarations when every parameterized edge has its own complete member proof. The bounded acyclic chain must consist of input declarations with original constructors and independent scopes: top-level declarations or named static members whose formals remain validated by the common scope plan. A higher override without a corresponding direct parent slot still needs a separate composed-member proof and fails explicitly. A same-package contract may retain package visibility or promote it when full parameter/result substitution matches and runtime descriptors do not change; it cannot weaken visibility or introduce a package bridge. The independent javac/D8/JVM oracle recovers seven classes and fifteen methods, including a static abstract root, its generic implementation and two generic descendants. It checks 46 identities and seven ordered failures through root, middle, interface and subclass views; unchanged nested generic reflection, package virtual dispatch, external default effects and one public bridge remain verified. A wrong same-erasure package parameter formal is rejected independently by NeverD and javac. Missing or opaque ancestors, raw nonempty superclass edges, missing constructors, cycles and unproven higher overrides remain rejected. Coverage stays `partial`.

Known external generic superclass chains use the same full parameter/result substitution after the common validator resolves their class and inherited method Signatures on demand. Each parameterized class edge composes its declaring scopes through a bounded acyclic graph; original constructors and independent scopes remain required. All class ancestors and their interface graph must be known external declarations. Dependency methods and existing interface/default implementations stay in their unchanged binary. Every inherited abstract interface slot needs a matching public class declaration, and input overrides without a class contract require a separate interface ownership proof. Original checked input bridges can be regenerated against a higher class contract. Missing or raw superclass edges, unbound formals, changed erasures, missing abstract implementations, newly overridden defaults and cycles fail explicitly. Constructor capture parameters and unused private or static dependency methods remain opaque. The independent API-24 javac/D8/JVM oracle uses a small authored entry, iterator and set view extending the SDK's parameterized `AbstractSet`. All three classes and twelve original methods recompile, and 20 identity checks plus six ordered failures match: inherited collection views, arrays, equality, hashing, iteration, streams, defaults, mutable fields and effects before exceptions. Original API generic reflection is retained; generated private exception helpers do not add public API. Swapping same-erasure generic return arguments with unchanged Code and descriptors is rejected by NeverD and javac. This fixture needs no bridge projection and reports `recovered`; separately checked regenerated bridges remain `partial`.

A second independent external-ancestor oracle keeps four authored API classfiles in an unchanged declaration JAR and recovers only four input classes and eight methods. The typed child override consumes a higher root declaration through an intervening generic superclass; javac regenerates its original public bridge. All 43 identity checks and seven ordered failures match through typed, raw, default and subclass views. It confirms inherited mutable fields, casts before target effects, defaults before casts, protected same-slot overrides and the full generic reflection. Changing only an input parameter Signature to a sibling formal, with Code and descriptors unchanged, is rejected by NeverD and javac. Dependency bytes and original input class hashes stay unchanged; coverage remains `partial`.

An independent API-24 default-override oracle retains six classes and fifteen methods, including a generic iterator, its source subclass and a two-level plain empty marker graph. Its 21 identity checks and seven ordered failures preserve inherited collection behavior, explicit `Iterator.remove` effects and failures, untouched defaults, `super.next` dispatch, integer parent-constructor calls and generic/marker reflection. Marker cycles, missing declarations, fields or methods, generic marker edges, reduced default visibility and added exceptions fail explicitly. Reordered same-erasure generic return arguments are rejected by NeverD and javac. No bridge projection is needed, so this fixture reports `recovered`.

Source declarations with an original `varargs` flag retain the final array dimension as `...`, including generic and multidimensional arrays. Descriptor and Signature erasures remain unchanged; a missing or nonarray final source parameter fails clearly. This source restriction does not rewrite or reject opaque dependency constructors whose compiler-added capture parameters follow the array. Ordinary invocation arguments keep their original array descriptors, and compiler-regenerated generic bridges keep their separate original non-varargs flag.

Known generic interfaces may inherit parameterized interface contracts through a bounded acyclic graph. Each edge composes its complete invariant arguments using the validated declaration scopes; equal diamond paths share a contract, while conflicting arguments fail. Every inherited public abstract member uses the same full parameter/result substitution and original-body bridge proof as a direct contract. An interface can inherit an abstract slot without a fabricated implementation. Unknown declarations, raw nonempty interfaces, inherited fields, method formals, unproven interface defaults and cycles remain unsupported. Source retains the original generic inheritance and reflection. The independent javac/D8/JVM oracle preserves eight classes and 24 method descriptors, regenerates eight original bridges, compares 52 identities, 18 ordered failures and three array allocations, and checks closed and class-owned arguments, generic arrays, mutable state and subclass dispatch through both diamond arms. A different same-erasure formal with unchanged Code and descriptors is rejected by NeverD and an independent javac override check; coverage stays `partial`. A second run places the four unchanged interface classfiles in a declaration JAR and recovers only four input classes and 20 methods, with the same eight bridges and all behavior/reflection checks. The common validator resolves only consumed external interface Signatures, checks their declaring scopes and exact parent/member erasures, and keeps unrelated dependency declarations lazy.

The empty, runtime-visible Java 8 `@java.lang.Deprecated` marker is preserved on classes, fields, methods and constructors. Parameter annotations, annotated static initializers, other visibility levels and element values such as `since` or `forRemoval` are rejected. CI separately compares the classfile `Deprecated` attribute and runtime annotations after recompilation, including unannotated controls and generated helpers; helpers remain outside the original method count.

DEX and smali also preserve the platform `@android.annotation.SuppressLint` on classes, fields, methods and constructors. It requires build visibility and exactly one string-array `value`; empty arrays, empty strings, repeated values, order and escaped character values are retained. Parameter annotations and annotated static initializers remain unsupported. An SDK-backed CI regression checks the class-retained annotations after recompilation and compares program behavior.

The built-in engine also preserves runtime-visible `@Retention`, `@Target`, `@Documented` and `@Inherited` on supported annotation declarations, emitting a real `@interface`. It permits top-level annotations and static member annotations with proven names, access and enclosing identity; local or anonymous annotation declarations and annotation class type parameters remain unsupported.

Annotation fields retain their public/static/final declarations. An annotation class initializer consisting of one verified static field read and copy can become a field initializer, after all encoded static values. This preserves values observed through initialization cycles. Literal fields use an effect-free, nonconstant expression so recompiling a field read cannot suppress initialization through constant inlining; no illegal static helper methods are inserted into the annotation declaration. The original `<clinit>` remains in coverage as `source-projected` with `projection_kind: "annotation-field-initializer"` and exact source/target field identities. Other annotation initializer shapes still fail.

Empty marker applications are supported on class, interface and annotation declarations only when the same analyzed class set contains an accessible, matching marker definition. Its Retention and Target must permit the actual application. A `SOURCE` declaration can be reconstructed, but a persisted application is rejected; `CLASS` or absent Retention requires DEX build visibility (`0`), and `RUNTIME` requires runtime visibility (`1`). Missing Retention remains distinct from explicit `CLASS`; absent Target remains distinct from an empty array, and target array order is preserved. DEX and dependency JAR declarations share the same target policy: Java 8 values are accepted, `MODULE` requires Java 9, and `RECORD_COMPONENT` requires Java 16. Recovery reports the highest required `minimum_java_release`; unknown and duplicate targets fail. `@Inherited` is preserved without copying inherited applications onto subclass declarations.

DEX annotation element declarations and defaults, nonempty applications, and custom annotations on fields, methods, constructors and parameters are retained. Values are checked against declarations in the complete input, including scalar kinds, strings, class literals, arrays and nested annotations. This covers the value shapes used by `kotlin.Metadata` and AndroidX `ReplaceWith`; it does not establish recovery of every Kotlin class body. Missing definitions, required values, invalid visibility and unsupported declaration shapes still fail explicitly. The smali reader retains its earlier marker and platform-annotation subset. Annotation element methods count as original abstract declarations; no replacement bodies are generated.

Straight-line constructors and static initializers can initialize blank final fields when every normal return has exactly one verified assignment. The source keeps `final`, the original assignment order and receiver identity. Other initialization paths still require a proof before publication. DEX class initializers may carry visibility flags, and synthetic debug positions may use line zero; debug parameter counts must still match the prototype.

Constructors may also contain a verified prologue of constants, register moves and writes to their own fields before the parent constructor call. Field writes remain before `super(...)`, including when the parent calls an overridden method that observes them. Declared zero-argument static or virtual/interface getters without declared checked exceptions may read initialized receivers; a typed temporary preserves each call exactly once and retains its result after delegation. Reference casts also use typed temporaries at their original positions, even when their results are discarded, so failed casts stop before later effects and successful casts remain available after delegation. Capture analysis tracks identity through casts of initialized local reference values. Getter and cast exceptions stop construction before the parent runs. These bodies require Java 25 flexible constructor syntax; `minimum_java_release: 25` appears on the method and aggregate coverage. Constants and moves alone do not raise the Java release requirement. Reads or casts of the uninitialized receiver, receiver escapes, unproven control flow and handlers across the prologue still fail.

For checked superclass constructors, recovery matches the original descriptor,
the incoming argument register and the declared exception types. An erased
parameter can retain its closed source type when the caller forwards that
argument directly or passes the result of its own checked static factory.
Both the factory and the superclass call must satisfy the constructor's
checked exception declaration.

DEX `SourceDebugExtension` and `MethodParameters` are preserved in `class_debug_metadata` and each method's `parameter_metadata` in the coverage report. They are metadata receipts, not a claim that javac recreates original source maps or synthetic parameter flags.

A constructor prologue may allocate a fresh local object immediately followed by its exact no-argument constructor. Shared planning retains the existing outer capture proof across that pair. Emission requires a concrete declared class without an implicit enclosing instance and an exact constructor without declared checked exceptions. The new expression stays in order before `super`, and a temporary retains its identity after delegation; it also requires Java 25. Mismatched receivers, unrelated constructors, intervening instructions, abstract targets and unproven declarations fail. The JVM oracle verifies first-use class initialization, constructor failure before parent initialization, one allocation per attempt, and the retained object and outer-instance identities.

Prologue static field reads use the same declaration and register proof in capture planning and source emission. Only exact static declarations without encoded constant values are admitted, with the matching opcode and a complete in-frame destination disjoint from the uninitialized receiver. Parameter registers can hold replacement values; capture origins and delegation aliases still require their separate shared proofs. A Java 25 typed temporary reads each field once at its original position, retaining class initialization and failures even when the value is unused, and retaining the snapshot if the parent constructor changes the field. The independent JVM oracle checks integer, wide and reference snapshots, outer capture, first-use initialization order, initializer exception identity and subsequent failed-class access. Unknown fields, instance fields, encoded constants and receiver-overlapping wide carriers fail.

A synthetic named-member constructor may also delegate through a bounded forward-only default-argument prefix. The common capture planner proves unchanged self and outer registers, integer values on every normal path, and one exact same-owner constructor call. Only integer constants, moves, mask operations, zero comparisons and a declared outer integer getter are admitted. The Java 25 emitter consumes that shared receipt and uses the ordinary operation verifier to preserve the branches, getter effects and failure order before `this(...)`. Unknown operations, backward edges, skipped initialization, branch entry into a getter result and changed outer identity fail. The independent JVM oracle checks 480 final values, 160 outer identities and 80 failures across all mask combinations, including null outer receivers and the original getter exception object. Full original metadata and inventory stay unchanged except reported member binding, synthetic flags and helpers; three malformed paths are rejected.

CI uses owned Java 8 fixtures through D8 and NeverD, recompiles all generated Java, and compares complete `Signature` metadata, reflection results, and behavior. These fixture checks do not qualify real applications for complete recovery.

### Linux and macOS

```sh
cmake --build build --target neverd

./build/bin/neverd mobile app.apk -o recovered-app
```

`NEVERD_JADX` and a `jadx` executable on PATH do not select the external engine; only an explicit `--jadx PATH` does. There is no automatic fallback. Quote paths containing spaces.

### Windows PowerShell

```powershell
& .\build\bin\neverd.exe mobile .\app.apk -o .\recovered-app
```

Multi-configuration builds may put the executable under `build/bin/Release/`. Follow the normal native-library deployment requirements for that build.

Nongeneric source interfaces retain static final fields and complete
method-local generic contracts through generic wrappers. Direct recovery and
linked input share the field declaration checker. Interface constants preserve
their encoded values through reported initializers compatible with Java 8.
The method-formal-interface-fields JVM oracle retains five classes and ten
methods, compares 34 values, 48 identities and ten ordered failures, preserves
field declarations and full class/method signatures, and rejects same-erasure
parameter/result scope changes with unchanged Code and descriptors.
Its bridge-implementation variant retains the same original method-formal body
and full signatures, measures the same runtime cases, and reports the bridge
reflection flag difference as `partial` coverage.
The same complete implementation admission now applies through known plain
class ancestors. A source parent can keep its original method-owned or fixed
parameterized ACC_BRIDGE implementation while descendants inherit it without
copied bodies or new methods. Complete method bounds and arguments, the nearest
provider, access, static/final roles and original exceptions still must agree.
Source-interface and immutable interface-JAR oracles cover ordinary bridge and
bridge/synthetic flags, retaining six classes/eleven methods or five classes/nine
methods. Each compares 928 identities, 416 states and 96 ordered failures across
inherited, interface, reflected, MethodHandle and original compiled caller paths,
including subclass dispatch, callbacks and aliases. Complete metadata changes
only by the two reported implementation flags and private helpers. Same-Code
bound, result and fixed-argument drift fails; javac independently rejects the
changed method bound. Coverage stays `partial` for the disclosed flags.
The fixed-implementations variant also checks a fixed wildcard contract and a
Signature-free static receiver delegation. Class-variable getters keep their
separate original proof. Complete method-formal comparison supports one known
plain interface upper bound; its separate JVM oracle preserves five classes and
four methods, 53 identities and four ordered failures, full metadata and an
unchanged external contract. Bound drift with identical Code/descriptors fails
both recovery and the independent Java override check.
The recursive-interface-bounds variant also compares the complete
`R extends Bound<? super R>` bound after method-formal normalization. Wildcard
variance and the bound's exact method scope are retained; same-erasure invariant
bound drift fails both checks. The JVM oracle preserves five classes, four
methods, 53 identities and four ordered failures against its unchanged contract.
The recursive-class-bounds variant retains `R extends Bound<R>` with its exact
invariant self scope. Its immutable interface JAR and five input classes retain
all six descriptors, full generic reflection, 53 identities and four ordered
failures; same-Code wildcard bound drift fails recovery and independent javac.
The recursive Enum default oracle retains `E extends Enum<E>` and `Class<E>`
through a generic child interface and receiver. Source and unchanged dependency
profiles preserve the original default owner, all metadata and zero bridges.
Twenty-four value combinations, 375 identities, 125 state observations and ten
ordered failures match the original JVM, including raw enum mismatches and
nulls. Same-Code changes to member or parent scopes fail both checks.

Named static plain ancestors keep an independent scope inside a generic
outer class. The original generic interface default stays in its declaration,
with complete method-owned bounds, parameter/result types and runtime method
inventory. `--nested-ancestor` and its `--dependency-ancestor` variant verify
constructor initialization, mutable aliases, virtual effects and ordered
failures through an immutable JVM harness. Captured or unknown scopes and
same-erasure generic contract drift fail; Signature-free consumers cannot
bypass a missing parent proof.

The `--dependency-method-formals` raw-parameter oracle retains abstract and
default methods with their own bounded type variables in a nongeneric SDK
interface. Input implementations keep alpha-equivalent original method scopes,
while the default body remains in the unchanged JAR. Full compiler metadata,
original method ownership, typed/interface/reflected dispatch, mutable aliases
and effects before failure must agree after recompilation. Same-Code changes
to an invariant parameter's method scope fail recovery and independent javac;
method-owned contracts cannot silently skip inherited-member validation when
their interface's class arguments are missing.

Its `--raw-dependency-defaults` variant keeps the SDK interface's class formal
and inherits its unchanged method-generic default through a raw Java view.
Full metadata and default ownership remain unchanged; 24 identities, 24 state
observations and six original SDK cast failures match the original JVM. A
same-Code generic overriding slot fails recovery and independent javac. Native
coverage applies the same rejection to a generic provider in a known ancestor.
Same-Code typed parameter counterexamples also fail against this erased raw
view; result-only class variables keep their existing covariance admission.

The owned raw-interface oracle preserves a comparator's original class-owned
parameter Signature while reporting the unique missing interface argument.
Direct, interface, reflected, reversed and method-generic SDK default calls
match the original JVM in 36 values, 98 identities, 46 state observations and
ten ordered failures. Full compiler metadata and runtime inventory permit only
the reported class interface Signature change, with zero new bridges or default
implementations. Same-Code method-scope shadowing and inconsistent parameter
counterexamples fail both recovery and independent javac.

Its captured-interface counterpart verifies the exact outer class formal,
retained outer-instance identity and mutable fields. The existing static-member
source projection reports every Object-bound generic erasure and capture flag
change; complete rebuilt metadata must match those receipts. Original runtime
entries, SDK default owners, alias updates and failures remain unchanged across
36 values, 194 identities, 46 state observations and ten ordered failures.
Native controls cover source and SDK interfaces, captured and owned variables,
both recovery entry points and reversed declaration order. Scope shadowing,
bound drift, unknown enclosing owners and fixed parameter conflicts fail.

The method-owned parameter-bridge oracle preserves an original checked key
adapter around a method with an `Enum<E>` self bound and invariant `Class<E>`
parameter. Source and unchanged dependency-interface profiles compare complete
compiler metadata and every runtime entry. Direct, interface and reflected
calls, an independent subclass override, null receivers, cast failures and
effects before original exceptions match in 40 values, 293 identities, 81 state
observations and 41 failures per profile. Same-Code self-bound and
invariant-argument changes fail recovery and independent javac; malformed or
missing original bridges and throws remain unsupported.

The `--ancestor-chain` variant adds a multi-level plain class chain with an
interface requirement, original generic ancestor methods and mutable state.
Source and unchanged ancestor-JAR profiles verify complete reflection,
construction, aliases, virtual overrides and failure effects. A same-Code
argument-count mutation fails both NeverD's referenced source/dependency scope
validation and independent javac. Signature-free receivers use the same known
ancestry and full member proof; no inherited method body is copied.

The `--ancestor-throws` variant retains the same fixed checked exception on the
interface, distant ancestor and input override. Complete method comparison
uses the existing validated exception compatibility receipt, preserving all
original declarations. Source and ancestor-JAR profiles verify reflection and
checked failure identity through direct, interface and reflected calls.
Different or added checked exceptions and unknown exception roles still fail;
original bridge exception-set equality remains strict.

Known external ordered-set ancestry keeps its original most-specific default
across runtime bridge slots in ancestor interfaces. Every bridge descriptor must
be witnessed by the complete substituted self-return proof. A certified integer
super getter and its original final virtual wrapper can also implement an
abstract class ancestor's identical plain integer slot. A unary generic super
constructor keeps the full substituted input, including wildcard variance,
through the common constructor-call receipt; method-formal and varargs overloads
remain unsupported. The ordered-sdk-set JVM oracle preserves one class and three
methods, compares 660 values, 1,495 identities and seven failures, and exercises
four covariant reversed-view entry descriptors through an immutable JDK 21 caller.
Original SDK bodies, mutable set state, comparator effects, constructor parameter
reflection and all other metadata stay unchanged. Changed generic arguments and
extra getter effects fail recovery. Only original bridge flags and helpers are
reported `partial` differences.

A final-owner Void target with a complete parameter contract can supply an
inherited boolean source result only after proof of its exact original static
prefix, exception allocation/constructor and unconditional throw. Its original
boolean bridge must preserve receiver, argument cast and unreachable result
conversion. A source alias retains the Void method and its generic signature.
The typed boolean method calls this entry and preserves the bridge conversion.
For a distinct erased parameter descriptor, javac supplies the inherited bridge.
The nonreturning-boolean-results JVM oracle checks all seven classes and 22
original methods, including callers of each descriptor. It compares 44 values,
67 identities and 38 ordered failures before and after rebuilding original bridge
flags. Scope drift and a body that returns normally fail recovery. The verified
report records the compiled entry and source projection for each original.

An abstract owner can retain a terminal Void method and its original integer
bridge through a disclosed source alias. The common proof requires the exact
private final String read, virtual Object.toString, exception constructor and
throw, followed by the unchanged bridge Number cast and intValue conversion.
Complete inherited parameter metadata must match, and known Void overrides
reject the alias. Allocation and class initialization still precede the field
read and call. The independently authored JVM oracle preserves four classes and
six methods, compares 480 fresh failures and ten external integer override calls,
checks all class/field/method metadata, and rejects normal return and same-erasure
parameter drift. Runtime names, binary ABI and bridge flags remain reported
`partial` differences; the original Void descriptor and full Signature stay intact.

An original void forwarder into an always-throwing Void method keeps both bodies
under a disclosed typed source alias. The common proof checks the final owner,
exact static or same-receiver private prefix, allocation/constructor/throw and
original receiver/argument delegation, then compares every complete inherited
parameter contract. The
source void parameter Signature comes from that checked typed target; known
callers use the same alias and exact visible lexical types. The captured anonymous
class JVM oracle preserves five classes and 23 methods through the reported
source binding, compares 38 values, 64 identities and 32 ordered failures, and
rejects scope drift and a normal-return body. Source names, bridge flags,
parameter reflection and anonymous/local metadata differences remain `partial`.

`test_mobile_android_plain_nonreturning_void.py` exercises the same shared
receipt before plain superclass validation. A final class without a class
Signature inherits through two abstract classes, retains a method-owned formal,
and implements a closed wildcard interface parameter alongside a `long` input.
The source and unchanged dependency-JAR profiles preserve seven classes/fifteen
methods and three classes/nine methods. Four original entries are projected.
The immutable JVM oracle compares 14 values, 2,246 identities, 1,120 states and
672 failures through seven routes, covering null receivers, exact prefix failure
identity and terminal allocation after the prefix. Both original bodies and all
other class, field, method and Exceptions metadata remain intact. Same-Code
flag/scope counterexamples and an independent javac generic clash must fail.

`test_mobile_android_closed_getter_forwarders.py` checks a raw getter bridge
whose virtual target has the complete closed result required by a superclass
and an interface. Source and unchanged dependency-JAR profiles retain six
classes/ten methods and three classes/five methods, respectively. The immutable
harness compares seven values, 1,036 identities, 337 states and 126 failures,
including inherited calls, external getter and entry overrides, mutable aliases,
null returns and exact exceptions. Only one original method's bridge/synthetic
flags are projected. All original Signatures, runtime names, fields, methods and
Exceptions declarations remain unchanged; no inherited body is copied. Same-Code
flag/result-scope counterexamples and an independent javac input must fail.

`test_mobile_android_nominal_boxed_results.py` checks original same-name long
and boxed interface return slots. The owned encoding changes method names and
bridge flags without changing instruction bytes, then preserves its runtime
inventory through DEX and the reported scalar source alias. Source and unchanged
parent-JAR profiles contain five classes/seventeen methods and four classes/
sixteen methods, respectively. Six original methods are reported as projected.
The immutable JVM oracle compares 665 values, 1,304 identities, 4,640 states and
30 ordered failures through eight boxed and three scalar routes. It covers wide
word boundaries, overflow, fresh allocation, null factory results and exact
scalar/factory exceptions. All metadata and exception declarations are checked;
changed wide arguments, mismatched factories, missing synthetic flags and an
inherited primitive contract must fail. Coverage remains `partial` for the
scalar alias, bridge flags and the broader return entry's extra stack frame.

A closed self carrier's erased scalar comparison may keep its exact original
short conversion and differently named instance target. The common proof checks
the complete generic input contract, checked receiver, conversion declaration,
register provenance and integer return. The source interface becomes raw to
retain the whole bridge body. The JVM oracle preserves four classes and nine
methods, compares 218 values, 75 identities and eight ordered failures, and
checks signed short extremes, conversion/target effects and full metadata.
Only the raw contract and bridge reflection flags are reported differences;
a Signature-only input-type change fails recovery.
The float profile uses the same checked bridge proof and retains its original
static comparison helper, including conditional NaN behavior. Its independent
JVM oracle retains four classes and ten methods, compares 3,946 values, 809
identities and eight ordered failures across NaN payloads, positive and negative
zero, subnormals, infinities and both original comparison modes. Complete
metadata differs only by the reported raw interface and bridge flags.

The long profile retains the same complete bridge and checks both conversion
words without overlapping the receiver. Its immutable oracle keeps four classes
and ten methods and compares 720 values, 1,567 identities, 5,916 states and 52
ordered failures through eight routes. Signed limits, distinct high/low words,
alias inputs, conversion/target/static-helper effects and exact exceptions match.
Raw interface and original bridge flags are the only disclosed metadata changes;
an altered long argument, an extra conversion and complete scope drift fail.

A source abstract interface can retain a fully substituted covariant result
when its original erased slot is absent. Complete inherited parameters, result,
visibility and abstract declarations must agree before the compiler-created
slot is reported separately from input methods. Nongeneric implementation
owners consume the same fixed source-interface contract without inventing an
original class Signature. The owned oracle preserves five classes and ten
methods, compares ten values, sixteen identities and four ordered failures,
and checks complete metadata and the rebuilt inventory against original slots
plus reported helpers. A Signature-only result-type change fails. The added
interface default bridge is measured and keeps coverage `partial`.

Bounded method-level covariance also applies to an original interface default.
The common bounded-result proof is combined with complete inherited parameter
substitution, method-formal scope comparison and the original forwarding body
before default selection. The owned JVM oracle preserves nine classes and
12 methods, compares 336 values, 553 identities and 20 ordered failures, and
checks narrow/wide calls, subclass overrides, bound-cast order, full metadata
and the exact reported throw helpers. Same-erasure input-scope drift fails both
recovery and an independent Java override check. Coverage remains `partial`.

Nongeneric source owners also prove nominal covariant return slots through
operation-local shapes of their actual runtime edges. These shapes add no
original class Signature. The authored JVM oracle preserves four classes and
nine methods, compares 43 values, 68 identities and four ordered failures,
and retains complete class/field/method metadata and inventory apart from
reported helpers. Generic echo parameters remain in their exact original
method scopes; changing only one same-erasure scope fails both checks.

Plain boolean bridges can retain a differently named same-owner virtual
method when the complete substituted inherited input is exactly its runtime
type. A final entry or final owner, exact ordered call words, original plain
target and three-instruction return body are mandatory. The shared scalar
proof serves direct and inherited class slots without changing any Signature.
The JVM oracle preserves three classes and seven methods, compares 242 values,
92 identities and 15 failures, and checks direct/interface/typed calls plus
an independently compiled subclass override. Complete metadata and inventory
change only for reported bridge flags and helpers. A Signature-only parent
argument change fails recovery and an independent javac check.

A closed parameterized input carrier can retain a plain reference conversion
whose original method has no Signature or method formals. Complete input types
stay in validation; the exact known conversion declaration determines the plain
result consumed by the target. No carrier formal is inferred. Open inputs,
generic conversion results and mismatched complete target parameters fail.
JVM reference and array/Unit profiles each preserve six classes, with nine/ten
methods, and compare 87/159 values, 58/75 identities and six/eight failures.
They retain conversion overrides, mutable payloads, nulls, raw float bits,
singleton initialization and all metadata apart from reported source edges,
bridge flags and helpers. Original input and harness bytes stay unchanged.

Known numeric conversions also retain an original void target followed by
its exact `Unit.INSTANCE` read. The value-carrier proof checks every cast,
virtual conversion, word width, receiver, target and post-call singleton access;
integer and boolean bridges retain their existing proof owner. The float JVM
oracle preserves five classes and eight methods, compares 213 values,
94 identities and 12 ordered failures, and checks raw float bits, non-Float
numbers, typed void calls and observable Unit initialization. Input Code and
harness bytes remain unchanged. Only reported raw-interface metadata, bridge
flags and helpers differ; a same-erasure reference-input counterexample fails
recovery and an independent javac override check. Coverage stays `partial`.

Double and long JVM profiles each compare the same 213 values, 94 identities
and 12 failures, including wide-register results and their exact Unit read order.

A final Signature-free owner can retain exact integer carrier bridges through
an independent source interface's complete closed parent contract. The shared
validator retains that original parent scope while projecting only the source
interface edge to raw Java; the implementing owner receives no new Signature.
Every inherited member and sibling implementation still needs its own proof.
The authored JVM oracle preserves four classes and five methods, compares 350
values, 333 identities and 12 ordered failures, and checks virtual `Number.intValue`
dispatch, non-Integer numbers, nulls and conversion/target exception identity.
An owned interface-name and access codec establishes the input before baseline
without changing Code or other metadata. Only the reported parent Signature,
bridge flags and generated helpers differ. A same-erasure parent argument change
fails recovery and an independent javac override check. Coverage stays `partial`.
The thirteen-argument range-call oracle separately compares 3,482 values,
8,923 identities and 18 ordered failures, including ten unchanged reference
arguments, both original integer conversions, their failure order and the
complete final call sequence. Input and harness bytes stay immutable in both
profiles; C++ tests also cover 21- and 22-argument contracts and bad packing.

An original plain Object bridge can retain a differently named primitive
getter followed by its exact static boxing call. Only a final owner, immutable
same-owner field read, known public static boxing declaration and complete
receiver/value/word-width provenance enter this profile. No generic result or
boxing semantics are inferred. The original call retains allocation, effects
and failures, and the report names the getter, boxing method and inherited
contract. The JVM oracle preserves four classes and 13 methods, compares 242
values, 176 identities and eight ordered failures, and checks signed zeros,
infinities, distinct float/double NaN payloads, dispatch and failure order.
An owned access-only codec marks the two original bridges before baseline
without changing Code or descriptors. Only their bridge/synthetic reflection
flags and reported helpers differ after compilation; full method-formal drift
fails recovery and an independent Java override check. Coverage stays `partial`.

A final source owner's exact private final field getter may return a container
of a known nominal subtype while an inherited getter requires the invariant
base element. The complete inherited graph proves the element ancestry, field
Signature and original receiver/read/return before projecting only that getter's
source result to the raw container. Fields, constructors and inherited contracts
retain their original Signatures; the report names every checked contract and
discloses return reflection and unchecked conversion. The JVM oracle preserves
seven classes and 12 methods, compares 47 values, 109 identities and 25 ordered
failures, and checks list identity, mutation visibility and original element
casts. Only the two reported getter Signatures and generated helpers differ.
An owned interface Signature edit establishes the input invariant contract
before baseline without changing Code or descriptors. A Signature-only final
field-type change fails recovery and an independent Java check. Coverage remains
`partial`.

An original Signature-free raw reference result may implement a closed
parameterized interface result through Java's unchecked return conversion.
Only unchanged runtime descriptors, fully plain original parameter contracts,
independent inherited scope and no method formals, open variables or throws
enter this profile. The report names every checked inherited result contract
and the compiler diagnostic; no Signature is invented. The JVM oracle retains
five classes and nine methods, compares 38 values, 35 identities and four
ordered failures, and checks exact metadata, allocation, forwarding effects,
failure identity and inventory apart from reported helpers. A Signature-only
parameter-type change fails both recovery and an independent Java override
check.

An original Signature-free method can also retain its raw parameters when its
whole signature is the exact erasure of an inherited member, as permitted
by [JLS 8.4.2](https://docs.oracle.com/javase/specs/jls/se8/html/jls-8.html#jls-8.4.2).
Each parameter must remain plain or use a validated parameterization of the
same raw class. Variables inside those arguments must retain their resolved
class or enclosing scopes. Plain results and runtime descriptors stay exact;
method formals, direct open variables, generic arrays and throws need separate proofs. An
implementation with its own Signature still needs complete argument equality.
The source and unchanged interface-JAR oracles preserve original captured
forwarding and superclass providers, all metadata and zero bridges. Thirty-six
value combinations, 444 identities, 156 state observations and twenty ordered
failures match the JVM, including alias mutation, subclass dispatch, captured
nulls and effects before failure. Inserting a typed Signature into either direct
or inherited providers keeps Code and descriptors unchanged but fails recovery
and independent javac. Coverage is `recovered`.

The `--scoped-nominal-parameters` source and unchanged interface-JAR variants
retain full nested wildcard contracts containing original class variables.
Direct and inherited implementations keep their Signature-free nominal
parameters, original bodies and complete metadata. In addition to the original
matrix, typed callers, reflection and MethodHandles compare 24 values,
114 identities, 66 state observations and 12 failures, without new bridges or
source Signature projections. Native coverage also exercises captured enclosing
method formals, input ordering and unresolved or partially typed counterexamples.

The dependency interface-fields variant also retains three original public
static final fields in the unchanged interface JAR. The common declaration
checker validates ownership, descriptors and field roles before direct or
inherited generic member checks. Plain dependency fields keep their original
library initialization; generic field Signatures need a separate proof. The
same 36 values, 444 identities, 156 state observations and twenty failures
remain matched. An immutable JVM wrapper additionally checks two constant
values, exactly one dependency initialization, mutable field aliasing and
original field ownership. No field or initializer is copied into input source.

The `--dependency-parent-chain` variant keeps four additional owned SDK classes
in the unchanged JAR. A plain concrete prefix inherits an abstract class whose
original superclass Signature fixes a deeper class formal to `String`.
The common edge composer validates all class arguments, roles and independent
scopes; original protected input overrides retain complete plain contracts.
The immutable JVM wrapper checks inherited method ownership and full formal
reflection, construction, mutable aliases, direct and reflected protected
dispatch, virtual SDK overrides and effects before the original exception.
Two existing SDK bridges remain in the library, with zero new source bridges.
Seven input classes and fourteen original methods recompile unchanged; the
wrapper adds seventeen identity checks, seven state observations and three
ordered failures to the original raw-parameter oracle.
An invalid superclass argument count preserves Code and descriptors but fails
recovery and independent javac. Raw generic holes, unavailable declarations,
cycles and input overrides of class-owned SDK formals require other proofs.

Adding `--abstract-fixed-prefix` keeps a fifth SDK class: an abstract prefix
above the original concrete provider. Its abstract flag creates no new member
obligation. Every intervening abstract class must retain concrete declarations
and only empty marker interfaces; the lower concrete provider remains mandatory.
The immutable JVM wrapper keeps the same value, identity, state and exception
matrix and checks the original abstract/concrete roles and provider ownership.
Unchanged input DEX is tested against SDKs with a new or reabstracted method;
NeverD and independent javac both reject those missing implementations.

The `--abstract-dependency-parent` variant admits a known plain abstract SDK
parent only after proving its complete original input providers. Nearest
abstract redeclarations block distant implementations. Public getters,
protected callbacks and alpha-equivalent method-owned generics keep their
full contracts, visibility and original ownership; no dependency method is
copied. The immutable JVM wrapper checks reflected protected dispatch,
method-formal ownership, inherited state and effects before the original
failure. Seven input classes and sixteen original methods recompile unchanged;
the wrapper adds eighteen identity checks, nine state observations and three
ordered failures, with zero bridges. A provider-formal mutation preserves Code and descriptors but fails
recovery and independent javac. Missing, abstract, private, static and
incompatible providers for the concrete consumer fail explicitly.

The `--deferred-abstract-child` variant adds independent plain abstract input
classes that inherit or redeclare the same nearest abstract obligations. Full
scoped parameter, result, visibility and fixed-throws comparison remains
required. Original abstract declarations keep their owners, and a nearer
abstract member continues to shadow a distant concrete implementation. An
immutable JVM subclass checks virtual and reflected calls, alpha-equivalent
method formals, mutable aliases and effects before the original exception.
A changed abstract redeclaration formal retains the absent Code and runtime
descriptor but fails recovery and independent javac. Complete class and member
metadata and runtime inventory recompile unchanged, with zero bridges.
Ten input classes and twenty-nine methods retain their complete metadata.
The wrapper adds eighteen values, 128 identities, eighty state observations
and sixteen ordered failures to the raw and abstract-SDK oracles.

The `--plain-class-covariance` variant adds original public and protected
return-only bridges to that abstract-parent profile. Known independent plain
class edges use local shapes without adding class Signatures. The shared
covariant-result validator proves complete parameters, nominal result ancestry
and each exact original forwarding body before either runtime sibling satisfies
a class requirement. The JVM oracle checks typed, erased, protected reflected
and subclass dispatch, mutable return aliases and ordered exception effects.
Missing bridges, changed flags or operations, reduced visibility and unknown
result ancestry fail. Original runtime entries and full metadata recompile
unchanged; bridge source projections remain reported as `partial`.
Eight input classes and twenty-two methods retain their original inventory.
The wrapper adds 48 values, 112 identities, 64 state observations and 24 ordered
failures to the raw-parameter oracle, including mutable result aliases and
typed, broad and protected SDK reflection through an independent subclass.

The `--covariant-siblings` extension gives each method two original erased
return entries through a second ancestor. The owned encoding omits the
middle class's compiler bridges before freezing the SDK; every other original
body and all concrete child entries remain unchanged. Each original body needs its own
shared receipt pointing to the same typed implementation; the exact required
ancestor receipt remains mandatory. Four public/protected bridges retain full
metadata and runtime inventory. The immutable JVM wrapper adds 48 values,
112 identities, 64 state checks and 24 failures through root reflection,
method handles and an independent subclass. Same-Code mutations remove the
bridge role or reduce visibility and must fail. Both regenerated bridge pairs
remain disclosed as source projections with `partial` coverage. Eight input
classes and twenty-four methods retain their original complete metadata.

`test_mobile_android_abstract_class_covariance.py` isolates an abstract middle
class with narrower public/protected returns and no original broader entries.
The owned encoding removes only its two javac bridges before freezing all
inputs; concrete descendants retain all four original forwarding bodies.
The complete abstract declarations survive unchanged, and the two compiler
additions are listed individually as `compiler-covariant-class-bridge` helpers.
Affected declaration rows report `class-covariant-runtime-bridges`; coverage
remains `partial` because reflection, rebuilt dispatch and stack frames can
observe the added methods. The immutable harness verifies root, middle and
concrete calls through reflection and method handles, independent overrides,
mutable result aliases, generic method scopes and ordered failures. Both source
ancestors and `--dependency-root` declarations are covered. Same-Code formal
and visibility changes fail; independent javac also rejects the formal change.
The source profile retains four classes and sixteen original methods; the
JVM oracle checks 216 values, 462 identities, 246 state observations and
36 ordered failures. The SDK profile retains two input classes and twelve
original methods with the same runtime observations.

`test_mobile_android_inherited_final_covariance.py` checks an original child
interface bridge whose narrower public final implementation remains in its
superclass. Both superclass and interface arguments must agree in the child's
full original scope. The common proof validates original virtual or super
forwarding, including optional nominal result casts. Java regenerates the
original erased bridge without adding a typed child method. The immutable JVM
harness compares direct, parent, interface, reflection and MethodHandle calls
through ordinary and independent subclass receivers, including nullable mutable
results, effects and exact exceptions. All original metadata and runtime entries
are compared; `--dependency-provider` uses unchanged parent/interface JAR
classes. Same-Code bridge-flag and generic-binding mutations fail; independent
javac rejects the incompatible binding. Coverage remains `partial` for the
regenerated bridge's compiler details.
The source profile retains six classes and ten original methods; the SDK
profile retains two input classes and five original methods. Both compare
96 values, 672 identities, 288 state observations and 144 ordered failures.

`test_mobile_android_inherited_self_carrier.py` checks this distinction between
original runtime contracts and Java invariant generic results. The owned input
changes only the interface result's wildcard encoding, preserving every Code
attribute and erased descriptor. The provider's original self-typed result and
all ancestor formals remain authoritative; its raw Java source result is
reported as `invariant-self-carrier-result-erasure` with unchecked compiler
conversion and changed generic reflection. Direct and inherited calls retain
one original provider, with no added bridges. An immutable caller, reflection,
method handles and an independent override compare mutable nullable results,
effects and exception identity. `--dependency-root` keeps the original carrier,
view and interface in an unchanged JAR. Wrong repeated formals fail without
publishing output, and independent javac confirms the unprojected invariant
source mismatch. Coverage remains `partial` for the disclosed generic change.
The source profile retains six classes and seven original methods; the SDK
profile retains three input classes and five original methods. Both verify
135 values, 285 identities, 150 state observations and 20 ordered failures.

`test_mobile_android_raw_self_interface_default.py` checks a source interface
that retains `Comparable<Self<T>>` and an original default with raw `Self`
parameters. Its exact original checked `Object` bridge witnesses the whole raw
subsignature while preserving the default's owner, body and scoped parent.
Two classes and four methods retain their complete metadata and runtime
inventory, with one reported regenerated bridge and no generic metadata
projection. The immutable JVM harness compares thirty values, 270 identities,
seventy state observations and forty ordered failures through typed, erased and
reflected calls, including an independent override, mutable aliases, failures
before and after default effects, rank dispatch and null receivers. A partial
`Self<String>` method Signature stays outside this whole raw-subsignature proof.
Removing only the original bridge flags preserves Code and descriptors but
fails NeverD; an independently authored explicit erased default fails javac.
Native controls also reject changed scopes,
method formals, abstract targets, missing bridges, wrong flags, invocation kinds,
casts and register provenance. Coverage remains `partial`.

The raw-self runner's `--inherited-consumer` variant adds an original generic
input class that implements the source interface and inherits its default.
The common validator's checked raw-parameter receipt is also consumed by default
selection; all class-edge arguments remain validated independently. The default
and its original bridge stay in their interface, with no new consumer entry.
Three classes and six methods retain their full compiler metadata and runtime
inventory apart from reported private helpers and the same one bridge projection.
The immutable harness uses the input class's original rank implementation and
checks the same thirty values, 270 identities, seventy states and forty failures.
Native controls reverse declaration order and reject removed or changed receipts,
conflicting ancestor arguments, altered providers and competing defaults.

`test_mobile_android_scoped_wildcard_defaults.py` checks an original generic
`putAll(Map<? extends K, ? extends Entry<K,V>>)` source default inherited by a
generic consumer. Its complete ancestor contract has the same runtime descriptor
and needs no bridge. Source and immutable root-JAR profiles retain five
classes/five methods and four classes/four methods respectively, apart from
reported private helpers, with no metadata or method projection. Each immutable
JVM profile compares 28 values, 256 identities, 52 state observations and 24
ordered failures through typed, raw and reflected calls, including an independent
override, null maps and receivers, mutable input aliases and a value cast after
key effects. Same-Code nested invariant argument drift fails NeverD and
independent javac. Native controls also reject changed wildcard direction,
unknown scopes, method formals, throws, abstract/static providers, competing
defaults, conflicting repeated edges and removed or changed common receipts.
Coverage is `recovered`.

`test_mobile_android_scoped_default_parameters.py` checks a generic source
interface's original `put(K, Entry<K,V>)` default against
`Root<K, Entry<K,V>>`. Its original checked second-parameter bridge also erases
the result to `Object`. The common full-contract receipt composes provider
formals into the consumer's own class scope, while the default and bridge keep
their original owner. Source and immutable root-JAR profiles retain five
classes/six methods and four classes/five methods respectively, apart from
reported private helpers and one regenerated bridge. No consumer `put` entry
or generic metadata is added. Each immutable JVM profile compares 43 values,
364 identities, 76 state observations and 33 ordered failures through typed,
erased and reflected calls, including an independent override, nulls, mutable
aliases and original exception identity. Same-Code invariant result argument
drift fails NeverD and independent javac; removing the original bridge flags
also fails NeverD. Native controls reverse class and method order and reject
incomplete receipts, conflicting parent arguments, changed scopes, casts,
invocation kinds, register origins, abstract providers and competing defaults.
Coverage remains `partial` because the original bridge is regenerated.

`test_mobile_android_specialized_interface_covariance.py` checks an original
`Byte` result and erased `Object` bridge against an interface specialized to
`Number`. Target selection uses the same complete scoped covariance and original
bridge-body receipt when no `Number` runtime entry exists. Complete invariant
`Carrier<Number,String>` parameters remain required. Source and immutable
interface-JAR profiles retain three classes/five methods and two classes/four
methods respectively, with one reported regenerated bridge and no generic
metadata projection. Each immutable JVM profile compares sixty values, 540
identities, ninety state observations and thirty ordered failures, including
independent overrides, typed/raw/reflected calls, null results and receivers,
mutable aliases, original exception identities and field casts after target
effects. Same-Code parameter argument drift fails NeverD and independent javac.
Native controls also reject missing bridges, changed flags or invocation kinds,
wrong register origins, unknown result ancestry and method-formal drift.
Coverage remains `partial`.

`test_mobile_android_nearer_generic_class_covariance.py` checks a covariant
`clone` over the SDK's parameterized map hierarchy. A nearer original public
plain declaration with no checked exceptions retains that contract over a
farther protected ancestor that declares a checked exception. The input's
original typed method and erased forwarding bridge still require separate
complete validation. One class and three methods retain their full compiler
metadata and runtime inventory, with one reported regenerated bridge and
`partial` coverage. The immutable JVM harness compares ten values, 140
identities, thirty state observations and twenty ordered failures through
direct, inherited and reflected calls, including an independent subclass,
shallow payload aliases, separate map storage, insertion order and exception
identity. Removing only the original bridge flags preserves Code and descriptors
but fails recovery; an independently authored return-only overload fails javac.
Native controls also check nearer abstract declarations, missing bridges,
changed results, static and final slots, unknown scopes and exception drift.

`test_mobile_android_static_return_overloads.py` checks two independent static
methods with identical parameter descriptors and distinct reference return
descriptors. A public final independent owner with no generic class scope or
inherited contracts can retain each original body under a collision-free source
name. The Object-returning entry keeps its name when present. Exact resolved
callers consume the same naming receipt; access flags, descriptors, Signatures,
Exceptions, argument evaluation and result checks remain unchanged. The report
records `incompatible-static-return-slot`, the conflicting local descriptors,
and the method-name reflection and binary ABI differences. Coverage remains
`partial`. The owned corpus preserves five classes and fourteen methods, compiles
every generated Java source, compares 208 fresh JVM cases with two attempts each,
and rejects eight independent wrong programs. Its two method-name constant-pool
edits occur before freezing the original corpus and preserve every Code attribute
and all bytes outside the constant pool.

Its `--synthetic-slots` profile additionally retains two original synthetic
entries. The same closed static-group proof owns both the renamed entry and the
entry keeping its name. Java cannot reproduce ACC_SYNTHETIC: each method receives
an explicit flag disclosure, and the unchanged-name entry records
`synthetic-static-return-slot`. All other access flags, descriptors, Signatures,
Exceptions and original bodies stay unchanged. The corpus sets only the two
synthetic flag bytes before freezing and verifies their preservation through D8.
The same 208 fresh JVM cases and eight wrong programs check both result slots.

`test_mobile_android_plain_sdk_return_slots.py` checks an original plain input
reference getter beside an inherited SDK integer getter with the same runtime
name and parameters. The complete known input interface family shares a source
alias, while SDK calls retain their original name and implementation. The JVM
oracle checks direct, interface, inherited and reflected dispatch, mutable aliases,
independent SDK overrides and ordered failures. Full metadata differs only by the
reported input names. Binary ABI and reflection name changes remain disclosed as
`partial`; no original bridge or new input method is invented. A changed getter
formal preserves Code and descriptors but fails recovery and independent javac.
Eight input classes and seventeen methods retain their complete runtime inventory.
The harness compares 31 values, 205 identities, 126 state observations and 33
ordered failures; only the three disclosed source names differ in full metadata.

The `--interface-covariance` variant adds a plain input interface result and a
known input subtype to the SDK-parent return-slot profile. A Signature-free
owner uses the same actual class shape in interface and class covariance proofs.
The original erased entry forwards to the narrow target and retains a proven
upcast to its interface result. Full parameters, nominal ancestry, flags,
receiver provenance and every original forwarding operation remain required.
Its immutable JVM wrapper checks typed, erased, interface and reflected calls,
null results, mutable aliases, independent overrides and exception identity.
A changed original cast target is rejected by the common bridge verifier.
Ten input classes and twenty-four methods retain their runtime inventory.
The wrapper adds 140 identities, seventy state observations and ten ordered
failures to the scalar/reference oracle. One original bridge and the three
source aliases remain explicitly reported as partial projections.

A nongeneric source interface can own fully parameterized methods without a
class Signature or method formals. Direct and inherited implementations consume
that original complete contract, including parameter and result variance.
The JVM oracle preserves six classes and 12 methods, compares 74 values,
80 identities and ten ordered failures, and verifies complete metadata and
inventory apart from reported throw helpers. Original class Signatures remain
absent. Changing only a lower-bounded parameter to an upper-bounded one keeps
Code and erasure unchanged but fails recovery and an independent Java override
check.

The same nominal covariance proof applies to an original plain source
interface default without class or method Signatures. A concrete class override
must retain its complete original result relation and exact erased forwarding
bridge; the shared inheritance graph still owns default selection. The JVM
oracle preserves four classes and nine methods, compares 45 values, 71 identities
and four ordered failures, and checks the narrow/broad override plus an
independent class inheriting the original default. Full metadata and inventory
remain exact apart from reported helpers. Same-erasure method-formal drift fails
both recovery and an independent Java override check.

An unannotated erased bridge may forward to a method with valid original
method annotations. Annotation validation and emission keep the typed target's
full types and values; the exact body proof still checks receiver, casts,
invocation and return. The bridge report identifies the typed annotation
source and discloses annotation copying by Java compilation. The owned oracle
preserves three classes and six methods, compares 49 values, 118 identities and
four ordered failures, and checks all metadata beyond that exact bridge
annotation difference and reported helpers. A bounded codec removes only the
owned input bridge annotation before baseline, retaining Code, descriptors,
constants and every other declaration. Same-erasure result drift fails both
recovery and an independent Java check. Coverage remains `partial`.

## Supported inputs and boundaries

| Input | Behavior | Important boundary |
|-------|----------|--------------------|
| `.apk` | Validate the complete ZIP, then analyze all root `classes.dex`, `classes2.dex`, and later numbered DEX files together | Code only; no resources or manifest decoding |
| `.dex` | Validate and parse DEX 035 or 037–040 with the built-in reader | DEX 041 and unsupported declarations or operations fail; renamed or truncated files are not valid bytecode |
| `.smali` | Analyze the supplied class | Referenced sibling classes are not implicitly loaded |
| Smali directory | Recursively collect `.smali` files and analyze them together | Include nested classes and dependent smali roots in the input directory |

To analyze an APK decode tree containing `smali/` and `smali_classes2/`, pass their common directory. Only `.smali` files reach the backend, but the entire supplied tree is validated and copied first; unrelated large assets still count against input limits. A compact directory containing only the relevant smali roots reduces work.

Split APKs are separate inputs. Each APK containing DEX can be processed independently, but this command does not merge an APK set; resource-only splits fail because they contain no root DEX. `.aab`, `.apks`, `.xapk`, `.odex`, `.oat`, and `.vdex` are not accepted mobile inputs. Backend support for some of these formats does not make them supported by this NeverD command.

APK resources, `AndroidManifest.xml`, assets, JNI/native libraries, and code downloaded at runtime are not recovered as Java. Extract a native `.so` separately and use `neverd decompile library.so -o library.c`. Encrypted or packed payloads must already be available as ordinary DEX/smali for this static workflow; no unpacking, device attachment, or protection bypass is performed.

For large methods, the Java emitter can group straight-line Dalvik instructions
between branch destinations. This reduces the bytecode space used by the
dispatch switch. When the method has exception handlers, the emitted code
records the original instruction position before each operation so an exception
still reaches the correct handler.

## Options and precedence

```sh
neverd mobile app.apk -o recovered-app --platform=android \
  --timeout=600 --max-files=30000 --max-bytes=4294967296 --json
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-o DIRECTORY` | Required | New output directory outside any directory input; never overwrite existing output |
| `--platform=auto\|android` | `auto` | Select Android explicitly or infer the platform from the input |
| `--jadx PATH` | Not set: built-in engine | Explicitly select the separately installed JADX compatibility adapter; no environment-based selection or automatic fallback |
| `--classpath JAR` | None | Read dependency declarations with the built-in Android engine; repeat for multiple JARs. Excluded from recovered-source and method counts |
| `--timeout N` | `300` | Positive analysis-time budget for the built-in engine; positive seconds per external backend process, including version probing |
| `--max-files N` | `20000` | Positive entry limit, including materialized directories |
| `--max-bytes N` | `2147483648` | Positive input, extracted-data, and final-output byte limit |
| `--json` | Off | Print the report as JSON rather than a human summary |

Non-default `--arch` selection, `--artifact`, `--metadata-only`, and a nonzero `--max-func` belong to iOS and are rejected for Android; explicit `--arch=auto` is accepted. There is no arbitrary backend-option passthrough. The explicit JADX adapter isolates config/cache/temp directories and does not import ambient backend settings or plugin configuration.

Input, extraction, and final output retain the configured file and byte budgets. The built-in reader and emitter also enforce bounded work and elapsed-time checks. External backend work areas allow up to three times the configured entry/byte budgets for staged inputs and intermediate output; logs are capped at 16 MiB per process. These are resource controls, not a sandbox. Raising one limit does not disable the others.

## Output layout and JSON report

```text
recovered-app/
  sources/                       recovered Java packages and classes
  metadata/android-methods.json  built-in method coverage
  report.json                    versioned inventory and recovery limits
```

Temporary inputs are removed. Nested classes can share an outer-class source file, so Java source count is not DEX class count. Generated methods may use a Java dispatch loop; they do not execute the original DEX or use a runtime bridge to it.

The built-in report embeds `android_method_recovery`, also written to `metadata/android-methods.json`. Every original method remains in the inventory. Its invariant is `method_count = recovered_method_count + projected_method_count + declaration_only_method_count + unrecovered_method_count`; an absent `projected_method_count` means zero, and `unrecovered_method_count` remains zero before publication. Original `native` and `abstract` methods have status `declaration-only` and are not counted as recovered bodies.

A bounded subset of named, noncapturing local classes can be emitted inside their exact enclosing static method. This requires an ordinary scalar method, a fieldless class directly extending `Object`, a real no-argument constructor, scalar instance methods, and verified object uses that do not escape the supported scope. Captures, unsupported modifiers, and unproved uses still fail explicitly.

Fieldless anonymous interface implementations allocated exactly once in their enclosing class initializer can be emitted as real Java anonymous expressions at that allocation. Their empty `Object` constructor is checked before the Java compiler supplies its implicit equivalent. Original methods and the allocating initializer are `source-projected` with `projection_kind: "anonymous-interface"`; the original class identity and allocation position are retained in `class_source_bindings`. Recompiled binary names remain unverified. Anonymous class identities used in other declaration signatures and multiple anonymous expression allocation sites still require additional source proofs.

Java enums retain real enum declarations and constant-specific anonymous bodies. A shared enum plan verifies literal constructor arguments, exact name/ordinal forwarding, constant write order, a complete ordered backing array, `values()` cloning, and `valueOf()` delegation. It accepts direct array construction and a verified private synthetic array factory. Private primary constructors retain their original bodies while Java supplies the first two hidden parameters; pure synthetic access constructors and constant-subclass constructors require exact forwarding proofs. A root access constructor with a distinct Java parameter list emits its own overload and delegates with the original arguments, including verified default-mask choices. An enum-constant subclass used as an access marker is recorded with its original DEX descriptor and its Java parameter type. Generic constructor signatures are matched after the proven hidden prefix. This follows the [Java enum declaration rules](https://docs.oracle.com/javase/specs/jls/se25/html/jls-8.html#jls-8.9).

Private enum constructors can retain acyclic `this(...)` chains ending at a verified Enum constructor. Each forwarding constructor must have a pure return continuation; target constructors own final-field initialization. One zero-argument static call may supply one delegation argument, consumed exactly once, and is emitted directly in that argument without rematerializing its result afterward. Checked-exception contracts for this call still require a separate proof. Original constructor descriptors remain in the inventory, including hidden name/ordinal parameters.

Primary enum constructor bodies can retain exception handlers when every protected region and handler begins after the verified Enum initialization. The ordinary body verifier checks exception-only handler entries and typed control flow. Handlers around the delegation, handler-bearing forwarding constructors, and handler-bearing blank-final initialization still require separate proofs. The execution oracle compares both normal and caught-exception paths and rejects invalid delegation coverage or normal entry into a handler.

Synthetic access constructors can select default arguments through bounded integer-mask guards. Each guard must skip one pure assignment to an ordinary parameter before exact delegation. The selected value is resolved from the literal mask at each verified allocation; effects, unknown masks, hidden-parameter changes, and other control flow fail. Both supplied and default values are checked by an execution oracle. These access-constructor descriptors remain in the original inventory as `source-projected`, although Java need not regenerate their binary signatures.

Final member subclasses of a proven enum can also supply constant bodies without an `enum` access flag. Their constructors must forward the hidden name and ordinal exactly, and their type identity cannot escape. Named implementations become anonymous constant classes, so bindings record changed names and reflection metadata. Typed class annotations have no declaration site on a Java enum constant body; their full source declarations and DEX visibility remain in `original_annotations` with `class_annotation_status: "report-only"`, rather than attaching them to the constant field.

An annotation stored on a proven enum constant may have Java targets that exclude fields. Such applications retain their complete values and DEX visibility in that constant's `original_annotations`, with `annotation_status: "report-only"` and an explicit reflection difference. The declared target policy stays intact. Permitted applications still appear in source; value types, retention, visibility and annotation accessibility remain validated for both paths. Target mismatches on ordinary fields still fail. The execution oracle preserves enum behavior and checks the reported absence of the unrepresentable annotation after recompilation.

Enum compiler-managed methods remain in the original inventory as `source-projected`. Bindings record every constant, ordinal, original implementation and constructor, while compiler-selected subclass names, access bridges and backing members remain unverified; coverage is `partial`. Other encoded static values needing an initialization-order proof, escaped backing arrays, unnameable subtype references and changed intrinsic behavior fail explicitly. The execution oracle checks constructor arguments, constant dispatch, `name`/`ordinal`, `valueOf` exceptions, independent `values()` arrays, `EnumSet` and constant-subclass reflection. Missing platform declarations use only the bounded enum name-scope contract; the `EnumDesc` member introduced after Java 8 still requires explicit dependency facts.

Final `int`/`long`/`String` encoded enum fields use Java constant initializers so their values exist before enum constant allocation. Original static field reads use generated accessors to retain active class initialization despite Java constant inlining. Bindings list `encoded_constants`, and accessors appear as `enum-encoded-value-read` helpers outside the original method denominator. The oracle checks reads during construction, interned string identity, and the first external field read triggering exactly one initialization. Reflective mutation of final constants remains outside this typed source projection.

Original instructions after enum-array publication can remain in a static initializer, with verified register values materialized at its entry. The constant prefix stays linear; the continuation can contain branches and loops verified by the ordinary body emitter. Blank-final assignments remain direct source statements between closed control-flow regions: a region may exit normally only at its next assignment or final return, and cannot skip or repeat a write. Bounded nested forward if/else regions may instead assign the same finals in multiple arms, provided both arms have identical write counts and each complete path writes every required field exactly once. The original arms and intervening effects are emitted directly, including later branches that do not write finals; stores stay at their original positions so callbacks can still observe partially initialized objects. Cross-arm entries, backward edges and unequal or repeated writes fail the same proof. Primary enum constructor bodies use the same proof after their verified Enum delegation. Handler-bearing initializers still require a separate proof. When those instructions use the backing array, an explicit copy is admitted only after a flow-sensitive ownership check follows array arguments through original static/direct callees and private final array fields. Element and length reads, null checks, cloning and runtime array-class queries are allowed; mutation, returning the raw array, identity observations and opaque or virtual calls receiving it are rejected. The report records `backing_array_projection: "verified-read-only-copy"`. This proof covers typed input code; private array identity and synthetic field metadata remain unverified under reflection. The independent array oracle checks continuation effects, shared table identity, bounds/null behavior, copy isolation and rejected escapes.

Computed enum arguments can retain static field reads, zero-argument virtual getters, and static factories whose arguments are typed literals or verified expressions. Wide call arguments must retain their original register pairs. A separately allocated object may be an argument after its exact constructor runs, with typed literal or expression arguments; uninitialized use and mismatched constructors fail. Its allocation and initialization remain in the ordered effect inventory. Composed expressions use bounded, separate register frames and retain each input effect in its original order, including when the constructed object becomes a factory argument. Reference assignment is shared with ordinary method verification and follows superclass and interface ancestry, including transitive interfaces. Integer and long addition, subtraction and multiplication can combine one computed expression with a matching-width literal; the ordinary emitter executes the original operation, including overflow, rather than folding it in the enum planner. Each argument receives a generated helper containing the original typed instructions, verified by the ordinary method emitter. The ordered effect sequence must match the original exactly once; reused expressions, omitted effects and reordered calls are rejected. Helpers are recorded as `enum-argument-evaluation`, outside the original method denominator. Array-valued computed arguments need a separate alias proof. A computed register still needed by the initializer continuation remains unsupported rather than being evaluated again. The stateful execution oracles verify getter order and call count, allocation-class initialization before constructor-argument reads, wide constructor arguments, subsequent factory calls, and the continuation.

Class-enclosed anonymous classes with fields, class declaration annotations, a non-`Object` superclass, or uses as declared types can be emitted as named static members of their original enclosing class. This includes field initializers inside anonymous local classes and stateless callback implementations reused as constructor access-marker types. A shared declaration-use plan determines naming consistently for validation, generic scopes and emission. Original constructors, annotations, capture fields, superclass arguments and concrete method bodies remain explicit. The `anonymous-to-named-member` binding records changed binary names and anonymous/member/static reflection metadata; coverage remains `partial`. Generic signatures that capture an enclosing type variable across the new static boundary are rejected. Fieldless direct `Object` subclasses implementing one interface, without declaration annotations or descriptor uses, retain the anonymous-expression path above. The callback/marker oracle verifies one shared class identity, both callable behaviors, the original constructor and all descriptors after the recorded rename.

Constructorless synthetic anonymous classes directly extending `Object` can also receive a named member binding. They may be empty markers or contain only static fields and methods, with no interfaces or class generic signature. No input instruction may allocate them or invoke their nonexistent constructor. Parameter references retain the marker type, and static utilities retain their original initialization bodies subject to the usual body proofs. Java requires a source constructor; the added private constructor is recorded only as a helper, and the report remains `partial` even when there are no original methods.

Method-local anonymous classes can instead receive a generated local source name while retaining explicit capture fields, constructor parameters and original method bodies in the exact enclosing method or constructor. Constructor-local declarations follow the enclosing constructor's `super(...)` or `this(...)` initialization. Anonymous classes nested in another anonymous local class's method keep both lexical scopes, including references to enclosing local classes and their declaring method's type variables. This reading-oriented projection records `projection_kind: "anonymous-to-named-local"`, keeps coverage `partial`, and reports known binary-name and anonymous/local reflection differences in `class_source_bindings`. Original descriptors and language metadata remain in the inventory; language metadata may refer to original names, and recompilation can add enclosing-instance fields or constructor parameters. Static member classes inside anonymous locals retain their declared ownership and require Java 16 while the owner remains local; their changed binary-name prefix is recorded as `member-of-projected-class`. Out-of-scope references, cyclic ownership, nonstatic members of local classes and unsupported bodies remain rejected. This projection is not an identity-preserving APK rebuild.

If other methods or declarations reuse an anonymous body outside its original local scope, it is emitted once as a named static member. Callers retain shared class identity and static storage; separate adapters can retain fields and constructor parameters of that class. The shared declaration is hosted by an accessible ordinary enclosing class, including a named class nested in an interface. Nested capture dependencies move with it when their original local scopes would no longer be visible. Cross-package references require the original class and every referenced named member suffix to be public. The source host must either be public through its enclosing chain or have a known public subclass in the same package through which the member can be named. In that case, `source_reference_class` records the inherited qualifier while the declaration stays in `source_enclosing_class`; host access is unchanged. This follows [Java member accessibility and inheritance](https://docs.oracle.com/javase/specs/jls/se25/html/jls-8.html#jls-8.5). Other access remains unproven. Coverage records `anonymous-to-shared-member` and the original enclosing method, while reporting changed names and member-class reflection: `getEnclosingMethod()` becomes null. A type variable from the former enclosing method cannot retain its original binding across the static boundary; the bounded metadata projection below covers only unconstrained `Object` formals from a proven enclosing scope.

Generic signatures inside an anonymous local class or its descendants may reference that local class, including constructor parameters such as `List<? super Self>`. These references follow the same lexical ownership proof as ordinary types. A declaration with an already proven shared-member binding may also use that exact local class in its generic arguments. A generic reference from an unrelated declaration or the enclosing method's header without that binding remains outside the local class's declaration scope and is rejected.

Two narrow generic metadata differences can be recorded in `generic_signature_projections`, retaining each declaration's original Signature string, source offsets and unchanged descriptor erasures. An anonymous class with no class formals may omit spurious type arguments only when they exactly match, in order, the same original enclosing declaration's type variables, and each binding is visible in the target anonymous class. Nested anonymous method scopes retain the declaring class or method identity rather than matching names alone. A local class moved into a shared static member can replace an out-of-scope variable with `Object` only when that variable has exactly the `Object` class bound and no interface bounds. Ordinary type-argument mismatches, arbitrary concrete arguments, unproven static-boundary captures and additional bounds still fail. These are reading-oriented source projections; generic reflection changes and coverage stays `partial`.

An anonymous field initializer emitted as a named static member can likewise project only an Object-bounded formal from its exact direct parent class. It must retain its original nonstatic enclosure; method formals, other enclosing classes and additional bounds remain rejected. `anonymous-member-type-variable` records the original parent declaration, Signature and unchanged runtime descriptor. The independent javac/D8/JVM oracle uses an unmodified real anonymous initializer and compares mutable outer state, 20 returned identities, nine ordered failures, all original methods and retained public owner metadata. A same-named method formal still binds to its own method. The two captured generic reflection differences and changed member name are reported; coverage stays `partial`.

A shared class may also retain the exact erasure of an enclosing class formal with one known, non-generic nominal class bound and no interface bounds. Parameterized bounds, method formals, unknown declarations and additional constraints prevent this witness. `shared-class-upper-bound` preserves the original Signature, exact declaring class and nominal source bound; it records the lost type-variable identity without changing descriptors or bodies. Shared scope projection runs within the common validator, and all source type-argument bounds are checked again afterward. The independently authored javac/D8/JVM oracle compares unchanged original Code bytes, one class identity across multiple factories and copying, distinct bound subclasses, mutable captures, 32 returned identities and 26 ordered failures. It separately confirms retained owner bounds and reported reflection differences; coverage remains `partial`.

The same shared-scope proof also accepts a method formal with one known, non-generic nominal class upper bound. `shared-method-class-upper-bound` records the exact original method scope and retains that bound in every projected use. Additional interface constraints, open or recursive bounds, and unknown declarations remain unsupported; all source type arguments are revalidated. The owned D8/JVM method-scope profile preserves original Code attributes and runtime descriptors, and checks 32 returned identities, 26 ordered failures, mutable captures, and the five disclosed reflection differences.

A separate shared method witness can retain a sole parameterized marker interface bound. The known top-level interface must directly extend Object, have one Object-bounded formal, and declare no fields, methods or inherited interfaces. Its argument must be `? extends` one known, non-generic ordinary class. `shared-method-marker-bound` copies that entire closed bound, including its wildcard and argument, into each projected field, constructor or result use; it records the original exact method and lost type-variable identity. Recursive or open arguments, other variance, member contracts and additional bounds fail. All source bounds are rechecked. The independent D8/JVM oracle retains all original Code bytes, every descriptor and the original method formal, and compares two implementations, mutable capture identity, 34 returned identities, ten ordered failures and the reported reflection changes.

A shared anonymous class can record a captured method formal with the sole bound `K extends Comparable<? super K>` as `Comparable<?>` in source. The real, known Comparable declaration must prove the exact single Object-bounded formal and abstract `compareTo(T) -> int` contract. `shared-recursive-interface-bound` preserves each original Signature, declaring method and descriptor while explicitly losing the self-relation and variable identity; dependent recursive type arguments must still fail the subsequent source-bound check. The same common witness admits only owned static `(K, K) -> int` calls with that one formal, no owner formals and no throws. Their original raw Comparable argument casts retain dispatch and add no check. Other generic calls remain unproven. The D8/JVM oracle compares 48 comparison result identities, six shadowing generic return identities, 37 ordered failures, distinct Comparable implementations, mutable captures, typed and bridge calls, null ordering and retained public method bounds. All original Code bytes and runtime descriptors remain unchanged; the wildcard and lost generic reflection are confirmed. Coverage stays `partial`.

A named inner class with a proven explicit outer-instance capture may retain a sole upper bound of its direct parent's class formal when projected as a static member. The bound must be one known, non-generic nominal class, or one known top-level, non-generic interface without a class bound. Parameterized or aliased bounds, additional constraints and other enclosures remain rejected. `static-inner-class-upper-bound` and `static-inner-interface-upper-bound` retain the original Signature and parent declaration while preserving the exact upper-bound type and runtime descriptors. Own method formals keep their original scope. All source type arguments are revalidated afterward. The independent javac/D8/JVM profiles compare virtual and interface dispatch, mutable outer state, 32 returned identities and 13 ordered failures each, retaining all original Code bytes and descriptors. They confirm retained public owner bounds and separately reported generic, owner-qualifier and static-member reflection differences. Coverage stays `partial`.

Constructor signatures may omit proven captured parameters stored before delegation into synthetic final fields. Typed local copies preserve the parameter origin, and a pure `this(...)` prefix can forward it through an acyclic chain to the constructor that stores it. The primary constructor owns final-field initialization; forwarding constructors cannot rewrite those fields. Continuations retain their original register state, including wide arguments and receiver aliases. The source retains every explicit parameter and its register position. `generic_signature_parameter_indices` maps each original generic parameter to its descriptor index, and `generic_signature_omitted_parameters` counts the captured parameters absent from that metadata. The alignment must be unique; repeated types with multiple possible mappings and parameters without capture evidence fail. This proof is shared by named inner-class projections and anonymous constructors. The delegation oracle checks both constructor depths, outer identity, final values, one superclass initialization per instance, and retained `long` values.

A final implementation of a single `P -> R` interface contract can retain a runtime parameter whose class or interface is a proven supertype of the concrete reference named by its Signature. The original body must start by checking that exact concrete type. A proven enum with runtime parameter `Enum` may instead check a declared interface that enum implements; preceding reads of the receiver's declared primitive fields remain in place. This preserves acceptance of other enums implementing the same interface. Both interface formals must have only an `Object` bound, the declared result must agree, and an exact synthetic bridge must forward the original argument to that body. If the bridge checks an intermediate type before the body's cast, both stages stay in place. The source method keeps its runtime descriptor; its metadata and the corresponding interface argument use that descriptor, recorded as `checked-erased-interface-argument` and `checked-erased-parameter`. All generic argument bounds are checked again after these projections to avoid declaration-order-dependent acceptance. Coverage remains `partial`, and the JVM oracles confirm changed generic reflection alongside unchanged descriptors, reference identity, both failed-cast stages, primitive capture values, side effects and null-receiver ordering.

The same exact unary interface contract can witness a concrete Signature return whose runtime descriptor is `Object`. The final implementation must extend Object, and every returned register definition must prove null, an incoming reference of the declared result class, or a freshly allocated instance with a matching constructor in the same basic block. Conflicting definitions, unknown producers, handlers and unverified bridges fail. The original input type arguments, including unbounded wildcards, must agree exactly. `checked-erased-return` and `checked-erased-interface-return` retain the original signatures and exact target method while rendering the result as Object; all interface argument bounds are rechecked. The independent JVM oracle compares typed and bridge calls, mutable values, wide fields, allocation identity, read and constructor exceptions, null arguments and receivers, unchanged method descriptors, and the reported generic reflection difference. Coverage remains `partial`.

A direct erased implementation can instead have the exact inherited `Object -> Object` method while its metadata narrows both input and result. The final Object-based class must implement one direct interface with two Object-bounded formals; its arguments must exactly match the narrowed method metadata, including nested wildcards. A linear body must first check the unchanged input against the parameter erasure and end by returning the adjacent result of a known member with the exact declared result descriptor. Additional interface slots must be ordinary zero-argument methods with exact erased implementations and no method formals. The source keeps that interface raw and the method's original Object descriptors; `checked-erased-direct-interface`, `checked-erased-direct-parameter` and `checked-erased-direct-return` record the full original signatures and exact method witness. Other methods retain their valid metadata. All parameterized bounds are rechecked after the interface projection. The independent D8/JVM oracle verifies direct and interface dispatch, mutable results, cursor identity and metadata, casts before effects, getter and reader exception identity, null ordering, and unchanged runtime methods. Generic reflection changes and coverage remains `partial`.

A final generic wrapper with one private final instance reference field can expose a static implementation whose first parameter carries that field's raw value. Static fields and declared empty marker interfaces, including a bounded acyclic marker hierarchy, do not change that instance representation. Class-variable metadata on a static method of any visibility can use `Object` in source only after an exact private synthetic constructor, synthetic boxing factory, and synthetic field getter prove the wrapper representation. Every class formal must have only an `Object` bound. Extra instance storage, missing or nonempty marker contracts, and generic markers remain unproven. Anonymous locals in that exact static method share the same class-variable decision while retaining its distinct method formals; method formals still shadow class formals. `static-carrier-type-variable` records the original declaring class and changed generic reflection; runtime descriptors, boxing identity, null behavior and method bodies remain intact. Independent JVM checks also preserve private access, marker inheritance, static initialization and call effects. Static declarations without the wrapper proof continue to reject out-of-scope class variables.

A separate checked return projection admits one class formal `S extends Base<S>` on that exact Object wrapper. A public static final accessor must have only the Object parameter, no own formals, handlers, declared exceptions or enclosed classes. Every returned value must immediately follow a copy of the unchanged incoming value and a check against the exact declared nominal bound; control flow cannot bypass that check. The source retains the recursive class bound and uses raw Base for the static return, recording `checked-static-carrier-return-type-variable`, the original Signature and exact method witness. Additional bounds and unconstrained returns still fail. The D8/JVM oracle compares distinct recursive subclasses, mutable values, wrapping identity, sentinel and exception ordering, null behavior and every original descriptor. Generic return reflection differs and coverage remains `partial`.

A synthetic anonymous adapter can recover missing variables in its stored callback's generic metadata when its original interface supplies concrete types. The adapter must be a final static member with one private final synthetic callback field, an exact constructor that stores the incoming callback, and a synthetic method that forwards every original argument unchanged to that field. The callback declaration must map its unconstrained formals directly to its method parameters and result; the implemented interface must supply a matching concrete generic signature. Matching includes nested arguments and variance, and each missing variable must resolve to one unparameterized reference type. Missing declarations, altered storage or dispatch, conflicting witnesses and unresolved variables fail. `adapter-captured-type-variable` reports each substituted variable and the exact interface method in `scope_origin`. Coverage remains `partial`: the independent JVM oracle checks unchanged callback, argument and result identities, call counts and exceptions, while confirming the recorded generic reflection difference.

Static members in these local projections require Java 16 or newer and raise the reported minimum release accordingly. A verified allocation may have pure register preparation between `new-instance` and its constructor call when the uninitialized object is not touched, control flow cannot enter the preparation, and no exception handler can observe moved allocation timing. Static or instance field reads and string constants between allocation and initialization are instead embedded as assignments in `new T(...)` arguments. Each read must be consumed exactly once in its original order without intervening aliases or writes; an instance field's receiver must remain unchanged through the constructor invocation. The assignment retains the register value for subsequent code. This keeps allocated-class initialization and allocation ahead of argument effects, string resolution and null field receiver failures. Other effectful preparation still fails.

`test_mobile_android_checked_static_super.py` checks a private static callback
whose parameter check precedes exact superclass dispatch. The owned fixture
changes one independently verified javac `invokevirtual` opcode to JVM-valid
`invokespecial`, retaining all other Code bytes, descriptors and metadata. The
actual changed class executes on the JVM before D8; its four-operation DEX body
is independently witnessed. Source and immutable superclass-JAR profiles retain
five classes/twelve methods and three classes/nine methods. Each profile checks
60 fresh JVM cases, repeated calls, independent subclass overrides, private
reflection/MethodHandle access, null checks, class initialization failures and
exact exception identities. The unmodified virtual-dispatch version must fail
the immutable observer. Original method flags and DEX metadata stay unchanged;
a reported private dispatch helper makes coverage `partial` and adds a stack
frame. Checks remain before helper receiver dereference, even when the checker
allows null to reach the original superclass operation.

`test_mobile_android_guarded_delegation_chains.py` checks genuine Kotlin default
constructors, checked overloads and a checked primary constructor with final
fields. Source and immutable superclass-JAR profiles retain three classes/eleven
methods and two classes/ten methods. Each profile uses 192 fresh JVM cases with
three input variants through direct, independent caller, reflection and
MethodHandle routes. Every default mask, unused mask bits, raw wide values,
receiver identity, mutable observations and exact failure identity are checked.
Null checks before an overload's delegation and after a synthetic constructor's
default calculations retain their different order. D8's original branches,
register copies and runtime annotation retention are independently witnessed.
An authored incorrect default mask must fail the unchanged observer. Runtime
entries and DEX metadata remain apart from the reported synthetic constructor
flag; CLASS-retained compiler annotations omitted by D8 are verified absent from
the input. Source requires Java 25, and coverage is `partial` for that disclosed
flag difference. Kotlin compilation uses the pinned compiler on JDK 21 separately
from source verification on JDK 25 or newer.

`test_mobile_android_constructor_parameter_reads.py` checks a genuine Kotlin
default constructor that overwrites its integer parameter register with a static
provider and then its virtual getter result. Source and immutable provider-JAR
profiles retain five classes/ten methods and two classes/five methods. Each uses
208 fresh JVM cases with two attempts, direct and immutable callers, reflection
and MethodHandles. Default and unused mask bits, external subclass dispatch,
input mutations, integer overflow, escaped partially constructed objects and
exact getter/parent/body failures remain observable. First initialization failure
and subsequent failed-class access are checked separately. Independent getter
bypass and duplication both fail the unchanged observer. Every original runtime
entry, DEX annotation, Signature and Exceptions attribute remains accounted for;
the reported synthetic constructor flag keeps coverage `partial` and the guarded
source prologue requires Java 25. D8's exact parameter-register reuse is checked
before recovery.

`test_mobile_android_allocation_field_before_nested.py` checks field reads
between outer and inner allocation using an unchanged javac/D8 fixture. Inner
class initialization mutates the field, so deferring or repeating the read
changes the result. Source and immutable inner-class-JAR profiles retain five
classes/nine methods and four classes/seven methods, complete metadata and
Exceptions, and the exact original method inventory apart from reported private
helpers. Each profile checks 32 fresh JVM cases through direct, independent
caller, reflection and MethodHandle routes, including repeated calls, null
receivers and values, original exception identity, and first/subsequent class
initialization failures. The original DEX allocation/read order is independently
witnessed; an authored late-read implementation must fail the unchanged observer.
The shared expression scheduler evaluates each predecessor once inside the outer
argument expression before inner allocation. Entry, handler, uninitialized
receiver and complete effect-order guards remain mandatory. No metadata
projection or newer source release is needed.

A named inner member of a projected class can retain its non-static nesting metadata when it has no original instance constructor or instance members, directly extends `Object`, and has no interfaces or class type parameters. Its static declarations require Java 16. The report identifies the added private source constructor as a helper and notes possible compiler-generated enclosing-instance storage.

A named instance member whose constructors store their first parameter into the same synthetic final outer-instance field before delegation can be emitted as a static member with explicit capture storage and constructor arguments. Leading static void checks may read parameters and local string constants without replacing the outer reference. A constructor signature may omit exactly that proven outer parameter; `generic_signature_omitted_parameters` records the mapping while the source keeps the raw outer parameter. Pre-super checks and capture writes require Java 25 and retain initialization and exception order. Calls need a known declaration without checked exceptions and cannot expose the uninitialized receiver. `inner-to-static-member` records changed modifiers, synthetic-field reflection and constructor metadata, with coverage still `partial`. An enclosing type variable with exactly an `Object` bound may use that erasure with an explicit `static-inner-type-variable` record. A parameterized owner such as `Outer<T>.Member` becomes `Outer.Member` only after its original ownership chain, argument count and bounds are verified; `static-inner-owner-arguments` records that change. Additional bounds and omitted parameters without a capture proof are rejected.

For these exports, the local methods and their enclosing method are `source-projected`, with `projection_kind: "named-method-local"`; coverage is `partial` even when the outer pipeline report says `success`. The recompiled binary names and access flags remain unverified. Java compilation can choose a different local-class binary name, so `class_source_bindings` preserves the original class, enclosing method, source path and local name with `binary_name_status: "unverified"`.

`generated_source_helpers` lists extra methods with the exact kinds `throw-helper`, `constant-helper`, `default-constructor`, and `field-initializer`. The last kind identifies an additional generated `<clinit>` that was absent from the original method inventory. These extras do not count toward the original-method denominator. Compilation or one matching binary name does not upgrade this status to complete recovery. The following shortened example has no projected methods:

```json
{
  "schema_version": 1,
  "status": "success",
  "platform": "android",
  "source": "app.apk",
  "input_kind": "apk",
  "backend": {
    "name": "neverd",
    "version": "1",
    "execution": "builtin"
  },
  "input_code_files": [
    "classes.dex",
    "classes2.dex"
  ],
  "dex_count": 2,
  "smali_count": 0,
  "java_source_count": 2,
  "java_sources": [
    "sources/example/Main.java",
    "sources/example/Peer.java"
  ],
  "logs": [],
  "android_method_recovery": {
    "schema_version": 1,
    "status": "recovered",
    "class_count": 2,
    "method_count": 6,
    "recovered_method_count": 5,
    "declaration_only_method_count": 1,
    "unrecovered_method_count": 0
  }
}
```

`input_kind` is `apk`, `dex`, `smali`, or `smali-directory`. `input_code_files` lists input bytecode names or smali paths, while `java_sources` and `logs` are relative to the output root. `source` is the input basename. Additional reconstruction limitations are included in real reports; retain them when presenting results to other tools.

When supplied, `classpath` lists dependency JAR basenames and declaration counts separately. Dependency methods are absent from `android_method_recovery`, and no dependency Java files are emitted. Dependency archives and extracted declarations are subject to resource limits and temporary files are removed before publication.

For automation, check the process exit code before consuming `status`, and keep the report outside the new output directory when redirecting stdout:

```sh
neverd mobile app.apk -o recovered-app --json > recovery-result.json
```

Successful native CLI runs return zero. Recovery failures return nonzero; `--json` reports handled failures with `schema_version`, `status: "error"`, and `error`. Argument parsing, native executable or library startup failures, and interruptions can instead report on stderr. Consumers must inspect the exit status first.

## Failure handling and troubleshooting

Publication is transactional: existing output is preserved, and failed staging output is removed. Unsupported operations, unresolved register flows, unrepresentable declarations, malformed exception handling, and exhausted budgets fail the built-in run instead of publishing missing method bodies. The external adapter also rejects nonzero exits, logged assembly/decompilation errors, duplicate-class omissions, incomplete-code markers, empty Java files, and missing Java output. Recovery success is not a proof of semantic equivalence.

| Symptom | Action |
|---------|--------|
| Unsupported DEX, instruction, declaration, or initialization | Read the explicit diagnostic; check the supported subset. Use `--jadx PATH` only if you deliberately choose the separate compatibility adapter |
| Invalid input or duplicate class | Correct the supplied bytecode/class set; unsupported bodies are not silently omitted |
| Timeout or budget exceeded | Narrow the input or adjust `--timeout`, `--max-files`, and `--max-bytes` within available resources |
| Output already exists | Choose a new output directory |

## Optional JADX compatibility adapter

`--jadx PATH` selects external JADX, not the built-in implementation. Install JADX 1.5.6 or newer with standard DEX/smali input plugins and Java 11 or newer. Obtain the complete [JADX distribution](https://github.com/skylot/jadx/releases/tag/v1.5.6), retain its `bin/` and `lib/` layout and included dependency licenses when redistributing it. Nothing is downloaded automatically. The adapter report identifies the actual `jadx` engine and detected version; it does not claim built-in method coverage.

On Windows, pass the distribution’s `.bat`/`.cmd` launcher or `lib/jadx-*-all.jar`. NeverD resolves the distribution JAR and invokes Java directly; application paths do not enter a command shell. `JAVA_HOME` or PATH selects Java. Successful adapter runs keep `logs/jadx-version.log` and `logs/jadx.log`; failed staging directories and logs are removed. Nonzero backend exits include a bounded log tail. Backend timeouts preserve the timeout message and append a bounded tail when captured log text is available. Launch failures and budget violations retain their own diagnostics.

```sh
neverd mobile app.apk -o recovered-jadx --jadx /opt/jadx/bin/jadx
python3 scripts/test_mobile_android_backend.py --jadx /opt/jadx/bin/jadx --neverd build/bin/neverd
```

## Verification and support depth

Python is used only by the development test harnesses below; built-in mobile recovery runs in the native C++20 CLI.

```sh
cmake --build build --target check-neverd-mobile
ctest --test-dir build -L NeverDMobileTests --output-on-failure
python3 scripts/test_mobile_android_internal.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_annotation_values.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_annotation_values.py --initializers --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_annotation_values.py --targets --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_classpath.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bridges.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bridges.py --anonymous --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bridges.py --marker --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bridges.py --marker-callback --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bridges.py --anonymous-base --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_marker_inheritance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_void_unit_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameterized_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_unit_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_unit_bridges.py --boolean --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_open_inheritance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameterized_unit_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_captured_sdk_inheritance.py --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_void_consumers.py --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_void_consumers.py --dependency-interface --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_void_consumers.py --inherited-interface --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_void_consumers.py --inherited-interface --dependency-interface --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_generic_default_class_bridges.py --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_generic_default_class_bridges.py --dependency-interface --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_protected_array_callbacks.py --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_protected_array_callbacks.py --dependency-parent --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_protected_array_callbacks.py --package-private --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_protected_array_callbacks.py --package-private --dependency-parent --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_anonymous_inherited_contracts.py --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_anonymous_inherited_contracts.py --dependency-parent --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_anonymous_inherited_contracts.py --fixed-throws --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_anonymous_inherited_contracts.py --fixed-throws --dependency-parent --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_enclosed_inheritance.py --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_ancestors.py --generic-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_ancestors.py --abstract-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_ancestors.py --abstract-parent --generic-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_cross_package_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_cross_package_ancestors.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_class_bridge_chain.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_class_bridge_chain.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_class_bridge_chain.py --inherited-members --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_class_bridge_chain.py --inherited-members --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boolean_result_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boolean_result_bridges.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_plain_callbacks.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_plain_callbacks.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_throws_empty_overrides.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_throws_empty_overrides.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_class_formal_object_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_class_formal_object_returns.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_formal_object_contracts.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_formal_object_contracts.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_formal_object_contracts.py --dependency-root --default-type --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_default_parameter_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_default_parameter_bridges.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_interface_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_interface_ancestors.py --dependency-interfaces --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_interface_ancestors.py --marker-chain --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_interface_ancestors.py --marker-chain --dependency-interfaces --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_interface_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_interface_ancestors.py --dependency-interfaces --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_public_protected.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_protected_closed_getter.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_interface_parent.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_generic_chain.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --source-interfaces --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --source-interfaces --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --dependency-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --source-interfaces --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --source-interfaces --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_deferred_abstract_interfaces.py --fixed-throws --dependency-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_generic_chain.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_ordered_sdk_set.py --java-home JDK21 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_owned_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_marker_default_overrides.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_method_formals.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --member-signatures --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --member-signatures --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --nonpublic-methods --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_dependency_defaults.py --nonpublic-methods --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_default_chain.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_default_chain.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_default_chain.py --class-interface-branches --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_external_default_chain.py --class-interface-branches --plain-default-override --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_mixed_external_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_mixed_external_defaults.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_incompatible_return_slots.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_incompatible_return_slots.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_disjoint_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_object.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_object.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_object.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_class.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_class_default.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_covariant_contracts.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_covariant_contracts.py --parent-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_scope_default_override.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_anonymous_guarded_super_adapters.py --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_anonymous_guarded_super_adapters.py --dependency-root --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_anonymous_guarded_super_adapters.py --nullable --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_anonymous_guarded_super_adapters.py --nullable --dependency-root --java-home JDK25 --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_superclass_scalar_adapters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_superclass_scalar_adapters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_reference_super_adapters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_reference_super_adapters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_guarded_default_super_adapters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_guarded_default_super_adapters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_marker_superclasses.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_marker_superclasses.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_erased_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --guarded-reference --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --inherited-guarded --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --inherited-guarded --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --guarded-integer --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --predicate-guard --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --generic-reference-getter --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --indexed-reference-getter --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --indexed-abstract-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --mixed --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_mixed_final_carrier_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_short_comparison_carrier.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_short_comparison_carrier.py --float --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_short_comparison_carrier.py --long --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nominal_boxed_results.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nominal_boxed_results.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --generic-result --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --boxed-integer --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_interface_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_interface_defaults.py --raw-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_upcast_result_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_upcast_result_bridges.py --generic-getter --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_upcast_result_bridges.py --ancestor-chain --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --result-only --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_reference_only_carrier_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_contained_consumer_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_contained_consumer_parameters.py --unbounded --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_object_lower_parameter.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --generic-input --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --zero-arity --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --standard-long --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --standard-double --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_long_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_character_getter_carriers.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_character_getter_carriers.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --standard-float --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_named_setter_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_parameter_variance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_comparator_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_parameter_covariant_result.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --named-primitive --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --primitive-array-result --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --reference-carrier --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --array-unit --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --scalar-unit --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_reference_result_boxing.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_method_formals.py --nominal-bounds --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_covariant_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_covariant_methods.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_covariant_methods.py --omit-interface-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nominal_interface_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_substituted_interface_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_boolean_forwarding.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --reference-carrier --parameterized-reference --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_value_carrier_bridges.py --array-unit --parameterized-reference --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_numeric_unit_bridges.py --profile float --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_numeric_unit_bridges.py --profile double --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_numeric_unit_bridges.py --profile long --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_integer_carriers.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_integer_carriers.py --ranged --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_primitive_getters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nominal_element_getters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_original_raw_results.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nearer_generic_class_covariance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_self_interface_default.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_self_interface_default.py --inherited-consumer --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_unchecked_implementations.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_unchecked_implementations.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --deferred-plain-slot --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_generic_interface_results.py --abstract-owner --deferred-plain-slot --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_void_return_slots.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_void_return_slots.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_object_lower_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_object_lower_returns.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_superclass_results.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_boxed_superclass_results.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_wildcard_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_inherited_wildcard_parameters.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_method_formal_members.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_method_formal_members.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_constant_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_constant_parameters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_inlined_interfaces.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_inlined_interfaces.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_inlined_interfaces.py --recursive-bound --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_inlined_interfaces.py --recursive-bound --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_package_generic_covariance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_package_generic_covariance.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_public_superclass_access_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_public_superclass_access_bridges.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_raw_class_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_raw_class_parameters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_plain_ancestors.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_plain_ancestors.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_primitive_bridge_copies.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_primitive_bridge_copies.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameter_return_covariance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameter_return_covariance.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameter_return_covariance.py --deprecated-target --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_parameter_return_covariance.py --deprecated-target --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_exception_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_exception_defaults.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_class_formal_implementation_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_class_formal_implementation_bridges.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --dependency-helper --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --reference-result --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --reference-result --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --inherited-predicates --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --inherited-predicates --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --omit-interface-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --omit-interface-bridge --dependency-helper --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --dependency-helper --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --reference-result --inherited-predicates --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_integer_static_comparisons.py --copied-receiver --reference-result --inherited-predicates --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --narrow --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --narrow --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --narrow --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_carrier_predicates.py --inlined-carrier --narrow --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_scalar_predicates.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_scalar_predicates.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_scalar_predicates.py --numeric --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_comparable_scalar_predicates.py --numeric --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_generic_ancestor_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_generic_ancestor_returns.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_generic_ancestor_returns.py --covariant-target --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_raw_generic_ancestor_returns.py --covariant-target --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_protected_scalar_array_targets.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_protected_scalar_array_targets.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_empty_original_bridge_throws.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_empty_original_bridge_throws.py --dependency-parent --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_exception_covariance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_exception_covariance.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_unchecked_covariant_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_unchecked_covariant_parameters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_unchecked_exception_narrowing.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_fixed_exceptions.py --exception-subsets --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_fixed_exceptions.py --exception-subsets --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_fixed_exceptions.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_fixed_exceptions.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_recursive_unchecked_callbacks.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_recursive_unchecked_callbacks.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_wildcard_defaults.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_wildcard_defaults.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_default_parameters.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_scoped_default_parameters.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_specialized_interface_covariance.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_specialized_interface_covariance.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_fixed_parameterized_methods.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_nominal_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nongeneric_nominal_bridges.py --source-default --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_annotated_bridge_targets.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_default_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_default_returns.py --default-override --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_default_returns.py --disjoint-class-roots --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_source_primitive_void_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --omit-interface-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --interface-default --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --inherited-default --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --reference-input --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --reference-input --omit-interface-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --getter-chain --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --deferred-class --reference-input --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --deferred-class --reference-input --omit-interface-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --inherited-entry --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --inherited-bridge --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_covariant_returns.py --class-bridge-override --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_private_producer_return_slots.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_final_getter_return_slots.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_superclass_getter_results.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_superclass_getter_results.py --integer-getter --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_abstract_integer_getter_bridge.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_abstract_integer_getter_bridge.py --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_abstract_integer_getter_bridge.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_abstract_integer_getter_bridge.py --dependency-ancestor --dependency-interface --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_return.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_nominal_return.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_return.py --class-ancestor --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nested_return.py --dependency-root --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_throwing_bridge.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_throwing_bridges.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_indexed_throwing_bridges.py --plain-superclass --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_nonreturning_integer_alias.py --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_enums.py --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_enums.py --named-constant --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_enums.py --constant-annotations --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_enums.py --recursive-bounds --android-jar PATH --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --computed-arguments --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --initializer-loop --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --wide-arguments --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --literal-factories --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --encoded-constants --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --delegating-constructors --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --object-arguments --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --object-constructor-arguments --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --conditional-constructor --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --constructor-handlers --neverd build/bin/neverd
python3 scripts/test_mobile_android_enum_arrays.py --default-constructors --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --guarded-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --generic-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --getter-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --delegating-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_guarded_constructor_captures.py --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_plain_integer_bridge.py --captured-member --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_plain_integer_bridge.py --captured-member --guarded-reference --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_plain_integer_bridge.py --captured-member --guarded-integer --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_indexed_throwing_bridges.py --captured-member --java-home JDK25 --neverd build/bin/neverd --android-jar "$ANDROID_JAR"
python3 scripts/test_mobile_android_constructors.py --cast-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_constructors.py --allocating-inner-members --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --generic --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --constructor --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --static-members --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --nested --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_local_captures.py --field-members --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --self-types --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --captured-signatures --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --class-captured-signatures --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --omitted-captures --java-home JDK25 --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --default-scopes --neverd build/bin/neverd
python3 scripts/test_mobile_android_kotlin_scopes.py --java-home JDK25 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_static_defaults.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_upper_bounds.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_upper_bounds.py --marker-method-bound --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_generic_bounds.py --neverd build/bin/neverd
python3 scripts/test_mobile_android_wildcard_inheritance.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_erasure.py --neverd build/bin/neverd
python3 scripts/test_mobile_android_return_erasure.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_direct_erasure.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_erasure.py --interface-parameter --android-jar SDK/android.jar --neverd build/bin/neverd
python3 scripts/test_mobile_android_checked_erasure.py --static-carrier --neverd build/bin/neverd
python3 scripts/test_mobile_android_bounded_carrier.py --java-home JDK17 --d8 PATH --neverd build/bin/neverd
python3 scripts/test_mobile_android_typed_adapters.py --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --member-host --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --public-facade --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --static-only-member --java-home JDK17 --neverd build/bin/neverd
python3 scripts/test_mobile_android_shared_anonymous.py --static-utility --neverd build/bin/neverd
```

The component and CLI tests check parsing, output contracts, and failure cleanup. The internal execution runners use a JDK (`java`, `javac`, and `javap`) and D8 to build independent DEX/APK fixtures and compile/run recovered Java; these are test dependencies, not requirements for built-in recovery. The annotation-value runner compares defaults and runtime reflection at five declaration sites, plus blank final instance and static field initialization. Run against the current build and inspect the results before claiming a case is verified. The separate compatibility runner additionally requires JADX and exercises that adapter. Fixture success does not establish complete recovery of arbitrary applications.

See the [mobile overview](mobile.md) for the related iOS workflow.

`test_mobile_android_anonymous_guarded_super_adapters.py` verifies an original
anonymous class in an instance factory, with boolean/integer guards forwarding
through original typed superclass adapters. Source and unchanged parent-JAR
profiles retain four classes/fifteen methods and three classes/eleven methods
before the explicitly reported local class name and constructor mapping.
The immutable JVM harness compares 102 values, 436 identities, 490 states and
20 failures across direct, reflected, MethodHandle and original caller routes.
It checks captured host identity, sibling instances, separate host objects,
parent state before callback failures and the different null behavior of the
erased guard and typed entry. Reflection constructs the rebuilt local with two
different host arguments, proving its explicit original capture remains the
one used by the body. The existing anonymous-class report discloses the named
local, bridge flags, capture field synthetic flag and added implicit enclosing
constructor parameter. A same-Code missing-final mutation and an independently
compiled adapter with an extra effect must fail. This profile requires JDK 25
for preserved pre-super capture assignments; coverage remains `partial`.
The `--nullable` source and parent-JAR profiles preserve the original null/type
condition join and forward null through the typed entry to the superclass. The
same observation counts include the resulting parent and captured-host effects;
wrong nonnull types still take the original rejection arm. Native counterexamples
also reject changed null origins, conditions, targets and unexpected metadata.

`test_mobile_android_local_nominal_scalar_guards.py` checks an original anonymous
view inside a static factory. Its full class and field scope is closed; an
independent two-formal method remains unchanged. Boolean and integer guards
check one concrete box, cast to a known parent, invoke the original effectful
byte/long unbox and call the original same-name scalar overload. Source and
immutable interface JAR profiles retain seven classes/eighteen methods and six
classes/fifteen methods. The JVM oracle compares 840 values, 360 identities,
600 states and 120 failures through direct, reflection and MethodHandle routes,
including mutable captures, arrays, wrong/null inputs, dormant failures and
ordered unbox/target exceptions. Full inventories and metadata must match
except reported local naming/capture/constructor and bridge flags. Same-Code
method-formal drift and a missing final flag are rejected; independent javac
also rejects the method drift. Coverage remains partial and original capture
constructor order requires Java 25.

`test_mobile_android_character_getter_carriers.py` verifies an original anonymous
class shared by two generic factories. Its erased key entry checks Character,
performs the original charValue call and forwards to a char method returning the
exact enclosing formal. A sibling View<Character> getter retains its complete
Signature and identity. Source and unchanged interface-JAR modes compare 336
values, 384 identities, 776 states and 48 failures across direct, reflected,
MethodHandle and original caller routes. Character boundaries, mutable captured
results, separate instances, getter effects and original exception identity are
covered. Inventories and metadata permit only the reported shared-member naming,
source-scope/interface projections and bridge flags. Three same-Code mutations
reject a different result formal, changed getter argument or missing synthetic
flag; independent javac rejects the getter conflict. Coverage remains `partial`.

Concrete class arguments may specialize method-owned dependent bounds, including
chains such as `<R extends T, S extends R>`, arrays and contravariant callbacks.
NeverD keeps the original erased entry only when its complete forwarding body
and the target's full method scope agree. The owned
`test_mobile_android_specialized_dependent_bounds.py` oracle covers source and
immutable dependency declarations, bound-position encodings, captured member
owners, reflection, method handles, callback effects and ordered failures.
Equal-erasure changes to the dependent chain or callback scope remain errors.

Closed primitive void consumers can retain a checked wide Number conversion
through inherited interfaces while keeping producer and metadata sibling
contracts. Their original parameterized interface graph remains available for
complete sibling validation after the disclosed raw source edge. The owned
`test_mobile_android_inherited_wide_consumers.py` oracle compares wide values,
Number conversion effects, exception ordering, immutable callers, full metadata
and unchanged dependency declarations. Conflicting producer arguments and
altered wide register words remain errors.

A final independent class may retain its original public erased bridge to a
protected void target with complete class-owned generic parameters. Recovery
reports the typed target's public source access and retains the original bridge,
full generic signature and checked forwarding. The owned
`test_mobile_android_protected_scoped_void.py` oracle covers generic containers,
scalar indexes, value aliases, source/SDK parents and ordered exceptions. It
rejects method-owned/foreign scope, invariant-container drift and altered
incoming register origins.

The same final-owner access proof covers class-scoped generic getters, scalar
results with generic parameters, and covariant reference results with original
wildcard parameters. `test_mobile_android_protected_scoped_results.py` compares
four original public bridge slots across typed, inherited, reflected and
method-handle callers. Full generic results and parameter variance remain
mandatory; visibility projection cannot justify a changed method scope.

A bounded generic provider in an abstract source parent may satisfy a child's
inherited contract through its original checked public bridge. Complete class
bindings and results are composed across the parent edges; recovery adds no
child virtual methods. The shared protected-target access proof rejects any input
subclass redeclaring the promoted runtime slot. The owned
`test_mobile_android_inherited_protected_provider.py` oracle keeps external
protected overrides, callers and dependency declarations immutable, and checks
virtual dispatch, bound casts, aliases, full metadata and ordered failures.

Protected getters may return arrays through an original public Object bridge.
The same nominal assignability relation checks primitive/reference elements and
array rank; full parent substitution remains mandatory. The owned
`test_mobile_android_protected_array_getters.py` oracle compares all eight
primitive array kinds, unchanged aliases, allocations and ordered failures under
source and immutable dependency parents. Changed elements/ranks and unproved
bridge casts remain errors.

Final protected void consumers can retain checked reference narrowing from a
known parent bound alongside unchanged scalar/reference arguments. Both Object
and bounded original bridge slots remain independently verified. The owned
`test_mobile_android_protected_mixed_consumers.py` oracle compares immutable
callers, long/double argument bits, casts, nulls and ordered callback effects,
with source and dependency parent chains. Missing bridge flags, altered bounds
and changed incoming register words remain errors.

That final protected void-consumer proof also admits original array parameters
through their checked erased reference slots. The shared assignability relation
retains array element and rank constraints. The owned
`test_mobile_android_protected_mixed_arrays.py` oracle covers stores through all
eight primitive array kinds, aliases, null receivers/arguments, bounds failures
and target exceptions; wrong element/rank inputs must fail before target effects.

The shared protected-target admission retains an original final method modifier
when its complete inherited contract and original public bridge are proven.
`test_mobile_android_inherited_final_protected.py` checks bounded provider scopes,
immutable callers, alias/exception behavior, full metadata and the absence of
new child virtual methods under source and dependency parents. Source widening
changes protected final to public final; it cannot permit an input override.

A final protected method may accept an original checked array input and return
a known reference class through its Object-to-Object bridge. Full input and
result substitutions are both required. The owned
`test_mobile_android_protected_array_reference.py` oracle verifies returned
identity, retained array aliases, constructor effects and exception ordering,
including source and immutable dependency parents. Wrong input/result types or
modified forwarding registers remain errors.

Checked primitive bridge conversions include Number.byteValue and
Number.shortValue, with their original Number receiver and corresponding boxed
contract. `test_mobile_android_narrow_number_consumers.py` compares both
conversions under source and immutable dependency interfaces, including overridden
conversion methods, Number's intValue fallback, truncation and ordered failures.
The inherited producer and metadata siblings still require complete validation.

Closed inherited void consumers may also retain an original primitive scalar
carrier. The existing carrier declaration proof owns its exact unbox method;
the bridge's original virtual call identifies a possibly differently named
typed implementation. `test_mobile_android_inherited_scalar_consumers.py`
compares source and immutable dependency carriers, both widths, unbox effects,
ordered exceptions and every inherited sibling contract. Unknown/mismatched
carriers, methods or incoming words remain errors.

Inherited producer bridges may preserve an original primitive result followed by
its exact known scalar-carrier factory. Fixed arguments and the complete
inherited result contract are checked before source erasure. The original
factory remains a call, including allocation, cached/null returns and failures.
`test_mobile_android_inherited_scalar_results.py` compares all eight scalar
kinds with source and immutable dependency carriers, six call routes and
unchanged consumer/metadata siblings. One descriptor policy governs original
unbox calls, result factories and inherited targets. Narrow types retain their
exact byte, short, char or boolean descriptor; float/double tests compare raw
bits, including signed zero, infinities and NaN payloads. No boxed object or
extra runtime entry is synthesized.

Protected generic consumers may narrow an inherited class/interface bound to a
known assignable implementation. Their own class variables, unchanged argument
positions and full inherited parameter signatures remain mandatory checks.
`test_mobile_android_protected_bounded_scoped.py` exercises both bound kinds,
source/dependency declarations and calls compiled before recovery. The public
bridge retains its bounded erased slot and original cast; only the typed method
visibility changes, with that difference disclosed in the report.

Nested return variance can also follow an original public erased bridge into a
protected parameterized target. Recovery first checks the unchanged bridge body
and complete inherited parameters, then applies the existing witnessed nested
variance projection. The report retains the original target Signature and names
the inherited declaration that requires the source spelling. The separate typed
visibility projection remains explicit. `test_mobile_android_bridged_nested_returns.py`
checks source/dependency parents, immutable callers, mutable iterator identities,
nulls, cast failures and ordered body effects.

An abstract generic source parent may retain a plain interface obligation
through another abstract source class. Recovery preserves absent declarations
or an original compatible abstract redeclaration; a concrete descendant must
still supply the complete public contract. `test_mobile_android_abstract_plain_chain.py`
checks both forms with source and dependency interfaces, original forwarding,
method/field/generic metadata, Exceptions attributes and immutable callers.

Protected covariant reference results can retain unchanged fixed parameters,
including mixed narrow/wide primitive words. The shared access proof applies to
the original public erased bridge and requires assignable result types plus
compatible source descendants. Final target modifiers remain final.
`test_mobile_android_protected_fixed_results.py` verifies abstract source owners,
source/dependency parents, immutable virtual callbacks, reference identities,
long extremes, double NaN payloads/negative zero and ordered failures.

Abstract generic classes can retain a bounded parameter declaration when its
broader compiler bridge is absent from the input. Recovery requires complete
scoped parameter equality across every ancestor, unchanged void results and
known nominal narrowing, and preserves the original abstract declaration.
The absent slot is reported as a `compiler-parameter-class-bridge` helper;
coverage stays partial because reflection, dispatch and stack frames may differ.
`test_mobile_android_abstract_bounded_parameters.py` compares immutable callers
and both original concrete bridges with source and dependency ancestors.
The `--concrete-parent` variant retains an intervening concrete parent method
that the bounded abstract declaration overrides; independent callers also
verify that original parent body through virtual and super dispatch.

Protected methods with class-owned generic array parameters use the same array
assignability as closed array consumers. Full inherited array element and result
Signatures remain mandatory, including class-variable ownership; erasure-only
matches do not qualify. `test_mobile_android_protected_scoped_arrays.py` compares
source/dependency parents, mutable input/result aliases, cast order and original
metadata outside the disclosed protected-to-public target access change.

The same scoped protected bridge admission accepts generic array results through
an original broader reference entry. `test_mobile_android_protected_scoped_array_returns.py`
checks an original provider call, nullable/covariant mutable arrays, result
identity and target/provider failure order. Both container and array element
Signatures remain exact, and the original bridge and target visibility change
are reported separately from unsupported or missing methods.

Generic superclass result factories also retain original array-valued targets,
including zero-argument entries. Recovery proves the exact virtual target,
unchanged array result, original static factory and complete ancestor result
contracts before using the existing disclosed Object source argument projection.
`test_mobile_android_boxed_array_superclass_results.py` compares source/dependency
ancestors, original arrays, cached/new/null factory results and ordered failures.
The original erased method remains explicit; changed bridge reflection flags
and superclass Signature are reported rather than treated as exact metadata.

A boxed result owner consumed through a nontrivial generic bound now keeps its
complete original superclass argument. The verified original target/factory body
is emitted in one reported typed result entry, and javac regenerates the original
erased bridge descriptor and flags. The report uses
`boxed-bound-superclass-result-bridge` and lists the new typed entry separately;
declared-method reflection and forwarding stack frames differ. Other original
metadata and all dependent bounds remain checked.

`test_mobile_android_bound_boxed_superclass_results.py` covers source and unchanged
SDK ancestors, an independently compiled bounded consumer, seven JVM call routes,
mutable array identity, factory caching/null results, and ordered failures. Run
it with `--dependency-parent` for the external ancestor profile. Native tests also
cover scalar/array carriers and bounded uses in classes, fields and methods,
through direct/linked and reversed input order, with malformed body and original
bound counterexamples.

`test_mobile_android_multiparam_boxed_superclass_results.py` covers boxed results
under `Parent<E,R,C extends Base<R>>`, protected parent/provider declarations,
and a direct parameterized interface also inherited through the parent. Both
source and `--dependency-parent` profiles keep all complete original arguments
and original metadata, recompile output and compare the immutable JVM harness.
The shared proof verifies each ancestor path and each independent boxed slot;
only the added typed entry and forwarding frame are source projections.

`test_mobile_android_array_carrier_superclass.py` compares original array-backed
value-carrier parameters through protected generic superclass methods with
void, integer and reference results. Both source and `--dependency-parent`
profiles retain all original ancestry and metadata, recompile every source,
and run the same immutable JVM callers. The report uses
`array-carrier-superclass-bridge`, records the exact unbox/delegate calls and
complete checked ancestor slots, and separately inventories each typed source
entry. Javac regenerates the original erased bridge; the new entry and
forwarding stack frame are explicitly reported.

Original compiler-generated narrow result bridges can be regenerated from an
unchanged generic superclass and fixed interface contract. The original bridge
must consist of a checked super call with unchanged arguments and return its
exact cast result. The broad superclass method remains inherited; recovery
does not create an extra overriding runtime entry. Coverage records the original
bridge target and interface dispatch as `generic-interface-bridge`. The owned
`test_mobile_android_inherited_checked_results.py` regression recompiles source
and dependency-provider variants and checks runtime method inventory, metadata,
wide parameters, external overrides, ordered exceptions and raw-parent calls
with heap-polluted results. Positive inputs come directly from javac and D8;
unsupported dispatch or incomplete generic contracts remain explicit failures.

Covariant overrides of declared Object methods retain their original generic
result, checked exceptions and erased compiler bridge. The shared covariance
validator requires the actual SDK root declaration and verifies the original
forwarding body; inherited children reuse that result. The owned
`test_mobile_android_object_root_covariance.py` test covers generic clone
results, shallow-copy aliases, external overrides and the unchanged failure
for a class without `Cloneable`, with exact runtime inventory and metadata.

An original covariant reference bridge and its typed target may both omit the
inherited checked Exceptions attribute. The shared closed-exception receipt
also covers this result-only descriptor change while retaining the complete
original type and forwarding proofs. Java regeneration keeps the typed entry
exception-free and can restore the inherited ordered Exceptions attribute on
the bridge; `source_bridge_declared_throws`, `bridge_exception_source` and
`metadata_differences` disclose that reflection difference explicitly. The
Object covariance regression has an owned metadata-only encoding profile that
checks this case with unchanged original Code, descriptors and constant pools.


`test_mobile_android_monitor_sparse_cleanup.py` compiles an owned generic Kotlin
synchronized block with the unmodified Kotlin 2.0.21 compiler and D8. Independent
SDK disassembly checks that a numeric constant after acquisition and the normal
release lie outside the original catch-all intervals. Source and frozen payload
profiles retain all runtime entries, Signatures, runtime annotations and
Exceptions, then recompile and run the unchanged monitor oracle: 6,000 concurrent
updates, four reentrant updates, two null-lock failures, four ordered failures
and six cross-thread release checks. Original compiler, dependency and caller
bytes are checked unchanged; an independently compiled unsynchronized body must
fail ownership checks. Native cases cover sparse numeric/padding gaps, both recovery entries
and reversed class order, while rejecting uncovered effects, changed locks,
overlapping wide writes, extra handlers and release bypasses.

The monitor runner's `--post-release` profile independently compiles a Kotlin
synchronized expression followed by a conditional callback and scalar return.
Both source and frozen-payload profiles retain the original inventory and
metadata. Forty cases use direct, immutable, reflected and method-handle callers
to compare 16 ordered failures, 20 callbacks outside the lock, mutable identity,
reentrancy before/after release and cross-thread acquisition. An independently
compiled callback moved inside the synchronized block must fail the unchanged
observer. Native cases also reject re-entry, cleanup coverage after release,
backedges, jumps into cleanup and normal release bypasses. D8's uncovered pure
conditional jumps remain typed and subject to the same complete CFG proof.


`test_mobile_android_erased_super_constructor.py` uses unmodified javac/D8
originals for a plain subclass forwarding `Sink<? super Child>` to a parent's
`Sink<Object>` constructor. An Object overload distinguishes exact target
selection. Source and frozen-parent/interface profiles each execute 28 fresh
JVM cases, with two attempts per case, through direct, immutable, reflected and
method-handle callers. The oracle checks parent/child initialization order,
first/repeated initialization failures, original callback/body exceptions,
late bridge casts, mutable identity and observations of the partially initialized
child. Complete runtime inventories, Signatures, fields, Exceptions and helpers
are compared. A separately compiled call to the Object overload must fail the
unchanged observer. Native cases check both recovery entries and reversed
class order, while rejecting unsupported bounds, variance, receiver changes,
access and overloads. No input bytecode or positive metadata is patched.


The monitor runner's `--multiple-release` profile uses unchanged Kotlin/D8
originals with three normal monitor exits and one exceptional exit. Two branches
return early; the remaining path performs a conditional callback after release.
Source and frozen-payload profiles each retain complete metadata and runtime
inventories, then execute 40 direct/frozen/reflected/method-handle cases with
16 ordered failures and 20 unlocked callbacks. Return values, early-return side
effects, reentrancy and cross-thread release are checked against the original.
Independent SDK disassembly counts every acquire/release and cleanup interval.
A callback moved inside the lock must fail the unchanged observer. Native cases
reject unreachable exits, changed locks, bypasses, cleanup re-entry and overlap
between locked bodies and post-release continuations.


`test_mobile_android_protected_allocation.py` compiles unchanged Java 17/D8
originals with allocation, a void argument-preparation call and a field read
inside one catch region. One handler returns the original exception; another
returns a static fallback reference. Source and frozen-payload profiles each run
40 fresh JVM cases with two attempts per case through direct, immutable,
reflected and method-handle callers. They compare allocation/argument/constructor
order, object identity, original failures, first/repeated target initialization
failure and first/repeated fallback initialization failure. Complete runtime
inventories, metadata, Exceptions and generated helpers remain accounted for.
An independently compiled preparation hoist must fail the unchanged observer.
Native cases additionally cover nested constructions, typed/catch-all handlers,
reversed class order and rejection of changed dispatch, catch reads of deferred
registers, handler re-entry and unsupported handler effects.

The `--handler-effects` profile retains two handler string literals, a logging
call with a discarded integer result, instance writes through a pre-allocation
receiver and a common return continuation. Both source and frozen-payload routes
run 48 fresh JVM cases with two attempts each. A preparation call replaces the
global receiver, proving the handler still uses its captured input. A failing
logging call proves later writes and the shared continuation remain skipped.
Independent late-receiver and early-write variants must fail the unchanged
observer, in addition to the preparation-hoist counterexample. Full original
inventories, metadata, Exceptions and helper accounting remain checked.


`test_mobile_android_null_constructor_arguments.py` compiles unchanged javac 8/D8
originals with a null generic argument in both same-class and superclass
constructor calls. A separate Object overload distinguishes exact target
selection. Source and frozen Owner/Sink profiles each execute 64 fresh JVM
cases with two attempts, through direct, immutable, reflected and method-handle
callers. They check original scalar/null arguments, mutable reference identity,
constructor/postlude failure order, first/repeated initialization failure and
complete runtime declarations, metadata, Exceptions and helper inventory. SDK
disassembly independently witnesses the null and original constructor target.
A separately compiled Object-overload call must fail the unchanged observer.
Native cases retain copied nulls and reject clobbers, non-null arguments,
branches, open type variables, method inference and varargs overloads.


`test_mobile_android_constructor_class_literals.py` uses unchanged javac 8/D8
originals with a reference class literal, an argument-preparation call, wide
constants and copied receiver words before a range superclass constructor call.
Source and frozen-parent profiles each run 36 fresh JVM cases with two attempts
through direct, immutable, reflected and method-handle callers. They compare
Class identity, mutable aliases, raw wide bits, original argument/body/postlude
failures, class resolution before argument effects, first/repeated initialization
failure and class literals for an already erroneous class. A separately packaged
runtime omits the literal target without changing remaining class bytes; missing
resolution must fail before argument preparation. Complete runtime entries,
metadata, Exceptions and generated helpers are checked after compiling Java 25
source. An independently compiled eager-initialization variant must fail the
unchanged observer. Native cases also cover array literals, capture origins,
same-class forwarding into the primary, reversed class order and malformed or
clobbered constructor prefixes.

`test_mobile_android_raw_interface_parameter_bridges.py` uses a genuine javac
bridge and encodes only its owner's class Signature as a raw interface edge.
It checks unchanged descriptors and Code bytes, then compares the compiler
original, encoded original and rebuilt source across 55 fresh JVM cases with
two attempts each. Source and frozen Api/Sink profiles exercise raw interface,
immutable caller, reflection, method-handle and concrete-target routes. The
observer checks casts before effects, nulls, incompatible incoming references,
closed generic callbacks, mutable identity, original exception identity and
exact call counts. Complete class and method metadata, Exceptions, runtime
inventory and helpers are checked after recompilation; only the disclosed
bridge/synthetic flags differ. Changed tail variance and arguments must fail
recovery, and a separately compiled double-call bridge must fail the immutable
observer. Native checks also vary source order, linked/direct recovery, copied
inputs and unused owner formals while rejecting unproved forwarding contracts.

`test_mobile_android_raw_scalar_tail_bridges.py` covers the same raw interface
contract when the first argument is converted by an original known final scalar
carrier getter and the target has a different name. The common bridge verifier
proves the cast, getter, scalar words, receiver, complete closed generic tail,
single target call and returned origin. It retains the actual getter's effects
and exceptions; the raw owner edge gains no inferred argument. Four source/SDK
and narrow/wide profiles compile all recovered Java and compare original entries,
full metadata and Exceptions across 80 fresh JVM cases with two attempts each.
Only the disclosed bridge/synthetic flags differ. Changed tail metadata must
reject, and eight independent wrong programs must fail the frozen observers.

`test_mobile_android_nested_carrier_invocations.py` compiles an original generic
factory with a three-argument callback type containing nested lower-bounded
carriers and a closed reference result. Source and frozen-provider profiles each
compare 56 fresh JVM cases with two attempts through direct, immutable,
reflected and method-handle callers. They check nested callback argument and
return identity, mutable aliases, raw wide arguments, nulls, callback cast
failures, provider/body failures and first/repeated initialization failure.
All original declarations, full Signatures, Exceptions and generated helpers
are accounted for after compiling the recovered Java. Metadata-only negative
fixtures retain unchanged Code bytes but must reject negative composed variance
and unused formals; an independently compiled repeated callback must fail the
unchanged observer. Native cases cover direct/inherited declarations, SDK
contracts, range calls, reversed input order and missing or bounded inner types.

`test_mobile_android_erased_factory_invocations.py` also exercises direct
returns of the producer's method formal. The full source and frozen-provider
profiles each compare 132 fresh JVM cases with two attempts: 36 factory cases,
64 element cases and 32 direct-seed factory cases. Element callers preserve both raw Object results and
original subsequent String casts. Observers check reference identity, mutable
array contents, null values and producers, exact callback count, raw wide bits,
provider postlude before a failing result cast, original exception identity and
first/repeated initialization failure. No unused result carrier is initialized.
Complete original runtime inventory, metadata, Exceptions and generated helpers
are checked after compilation. Metadata-only negative producers and extra
formals retain unchanged Code and must reject. Independently compiled duplicate
factory callbacks and element reads must fail the unchanged observers. Native
coverage includes source/SDK contracts, invariant/covariant producers, inherited
facades, range calls and reversed input order.
The direct-seed profile preserves a formal input without requiring a callback:
it checks null and mutable seed aliases, fresh carrier identity, exactly one
allocation, wide bits, body/constructor failures and first/repeated provider or
carrier initialization failure. Removing the seed formal, replacing its result
argument or adding an unused formal must reject with original Code intact. An
independently compiled duplicate allocation must fail the unchanged observer.

`test_mobile_android_mixed_carrier_invocations.py` supplies an Object array,
a carrier mixing its method formal with a closed Object argument, and a
covariant producer. Source and frozen-provider profiles each compare 52 fresh
JVM cases with two attempts through direct, immutable, reflected and
method-handle callers. They check array/reference alias identity, array reads
before callback mutation, raw wide bits, null and empty arrays, null callbacks,
saver cast timing, mutable producer results, exact original failures and
first/repeated initialization failure. Complete runtime inventory, full
Signatures, Exceptions and generated helpers are checked after recompilation.
Metadata-only formal-array, negative-variance and extra-formal variants preserve
Code and must reject; an independently compiled double saver must fail the
unchanged observer. Native cases additionally cover primitive and multidimensional
plain arrays, closed-leaf variance, source/SDK declarations, range calls and
reversed class order, while rejecting generic arrays or incomplete type trees.

`test_mobile_android_seeded_carrier_invocations.py` uses an original generic
factory that takes a seed of its method formal and a callback consuming and
returning the same formal. Source and frozen-provider profiles each compare 48
fresh JVM cases with two attempts through direct, immutable, reflected and
method-handle calls. They check incoming and returned identity, mutable seed
aliases, wide argument bits, callback counts, nulls, cast timing, body failures
and first/repeated initialization failures. Complete original runtime entries,
Signatures, Exceptions and generated helpers are checked after recompilation.
Metadata-only negatives preserve Code while removing the seed formal or adding
an unused formal; both must reject. An independently compiled double callback
must fail the unchanged observer. Native cases vary seed position, source order,
SDK/source declarations, direct/inherited references and range calls, while
rejecting changed bounds, variance, slots, visibility and dispatch.

`test_mobile_android_seeded_void_carriers.py` exercises a generic void provider
with a direct formal seed, a contravariant callback with a fixed result type,
and a fully closed sibling callback. Source and frozen-provider profiles each
compare 60 fresh JVM cases with two attempts. Immutable callers check callback
order, mutable seed and result identities, nulls, wide flags, the original casts
after each callback, exact body failures and first/repeated initialization
failures. Inherited static references must initialize only their declaring
provider. Original class/method/field metadata, Exceptions, runtime entries and
helpers are checked after compilation. Metadata-only missing-seed,
missing-callback-formal and extra-formal variants retain identical Code and
must reject. An independently compiled duplicate callback must fail the original
observer. Native cases also cover virtual/interface dispatch, argument order,
range calls and unsupported bounds, unknown leaves, wildcards and member shapes.

`test_mobile_android_contravariant_carrier_returns.py` checks an original generic
method accepting a lower-bounded unary carrier and returning the same carrier
type invariantly. Source and frozen-provider profiles each compare 40 fresh
JVM cases with two attempts through immutable, direct, reflected and
method-handle callers. The provider reads and mutates its input, then returns
an independently supplied carrier; it is not an identity operation. Tests keep
distinct/aliased/null results, mutable payload identities, late casts, original
exceptions and declaring-class initialization. Complete metadata, Exceptions,
runtime entries and helpers are checked after recompilation. Metadata-only
missing-formal, variant-result and extra-formal cases preserve Code and reject.
Separately compiled bypassed and repeated provider calls must fail the same
observer. Native cases also cover virtual/interface calls, source order, aliases,
range calls, changed carrier types and bounds.

`test_mobile_android_unused_formal_invocations.py` exercises a static method
whose sole Object-bounded formal is unused in its Object parameter and return.
The declaring class has its own same-named formal. Source and frozen-provider
profiles each compare 36 fresh JVM cases with two attempts, preserving the
separate generic scopes, actual returned identity, mutable arguments, call
order, exact exceptions and first/repeated initialization failure. Full runtime
inventory, metadata, Exceptions and generated helpers are checked after source
compilation. Bound, used-slot and extra-formal negatives preserve Code and
reject; independently bypassed or repeated calls fail the unchanged observer.
Native tests vary source order, range calls and source/SDK declarations while
rejecting changed bounds, slots, access, throws and dispatch.

`test_mobile_android_unused_formal_names.py` uses an unmodified Kotlin compiler
to produce a public static nonsynthetic method with an illegal Java name and an
unused method formal. Source and frozen-provider profiles each compare 44 fresh
JVM cases with two attempts. A stable wrapper, immutable caller, reflection and
method handles exercise the original body and its disclosed source name,
including separate caller/provider initialization failures, nulls, mutable
aliases, opaque results and exact original exceptions. All access flags,
Signatures, runtime annotations, Exceptions and original entries remain exact
apart from the reported method name. D8's dropped CLASS-retained annotations
are independently checked and their full original payloads recorded. Same-Code
changed-signature variants reject; bypassed or repeated provider calls compiled
independently must fail the unchanged observer. Reserved-name collisions, source
order, range calls, unsupported scopes and enclosing-method metadata have native
coverage.

`test_mobile_android_private_instance_carriers.py` exercises an original private
instance void method and its own private static nested generic carrier. Source
and frozen-callback profiles each compare 68 fresh JVM cases with two attempts.
Immutable, direct, reflected and method-handle callers check receiver identity,
private call counts, callback order, mutable input/result aliases, wide bits,
null receiver/carrier/callback/result, cast and bounds timing, original body
exceptions and separate owner/carrier initialization failures. Private access,
InnerClasses metadata, complete Signatures, Exceptions and original entries
remain exact after recompilation. Metadata-only extra-formal, wildcard and
missing-formal variants keep Code unchanged and reject; independently omitted
or repeated private calls must fail the unchanged observer. Native cases also
cover range calls, public source/SDK carriers, source order and invalid owner,
nesting, visibility, dispatch, bounds and throws relations.
The alias counterexamples supply valid receivers and original constructors,
then require rejection specifically for private access after member lookup.

`test_mobile_android_paired_void_carriers.py` checks a public abstract method
with two Object-bounded method formals, one invariant binary carrier and one
direct seed. Two original implementations have different observable effects;
source and frozen-base/carrier profiles each compare 112 fresh JVM cases with
two attempts through direct, immutable, reflected and method-handle callers.
They preserve actual virtual dispatch, mutable aliases, input bridge-cast and
result-cast timing, wide bits, nulls, exact exceptions, constructor failures and
failed class initialization. Full original inventories, Signatures, access,
Exceptions and helpers are checked after compilation. Missing-seed,
repeated-formal, variant-carrier and extra-formal mutations keep Code unchanged
and reject. Independently bypassed or repeated calls must fail the unchanged
observer. Native cases cover static, virtual, abstract and interface declarations,
source/SDK carriers, argument and declaration order, range calls, additional
seeds and a variant mapper beside a valid pair.

`test_mobile_android_nullary_carrier_factories.py` checks a static no-argument
factory returning an invariant carrier of its sole Object-bounded method
formal. Source and frozen-provider profiles each compare 96 fresh JVM cases
with two attempts, through direct, immutable, reflected and method-handle
callers. The factory mutates hidden input state and returns a supplied list;
its effects and mutable, immutable, empty, null and aliased results remain
observable. Separate raw-result and checked-element callers preserve original
cast timing, exact failures and repeated initialization failure. An inherited
facade must initialize only the true declaring provider. Original metadata,
Signatures, Exceptions, inventories and helpers remain exact after compilation.
Bound, variance, closed-result and extra-formal mutations preserve Code and
reject; independently bypassed or repeated calls fail the unchanged observer.
Native cases also check source/SDK declarations, order, aliases, range calls,
missing carriers, generic array results and unsupported instance factories.

Its `--paired-result` profile retains two independent Object-bounded formals in
an invariant `Map<K, V>` result. Source and frozen-provider runs keep five
classes/thirteen methods and two classes/six methods, respectively; each compares
192 fresh JVM cases with two attempts. Separate key and value callers check both
late casts, empty-map lookup versus iteration, null keys and values, mutable input
aliases in either slot, and a shared key/value identity. Mutability, exact factory
effects, inherited initialization and all original metadata remain checked. A
same-Code repeated-formal result is rejected alongside bound, variance, closed
slot and extra-formal mutations. No empty-map or singleton behavior is assumed.

The `--closed-object-tail` profile retains a sole method formal in the first
invariant slot and a literal invariant Object in the second, as in
`<K> Map<K,Object> make()`. The complete known carrier still requires two
Object-bounded class formals. Only a real public static factory in an independent
nongeneric public owner receives this call receipt; its original body and returned
reference remain authoritative. Source and frozen-provider runs each compare
192 fresh JVM cases twice, compile every generated source, and retain the full
original metadata and inventories. Six same-Code Signature controls reject
reversed slots, extra formals, changed bounds, variance and a non-Object tail.
Independent bypass and repeated calls fail the unchanged observer.

`test_mobile_android_contextual_carrier_factories.py` checks
`<K> Map<K,String> make(List<K>,Config)`. One complete invariant input carrier
binds the sole Object-bounded method formal; a known nongeneric configuration and
fixed nominal result tail keep their original types. It shares the complete
result-slot proof with the nullary profile, whose tail remains restricted to
Object. Source and frozen-provider routes each compare 216 fresh JVM cases
twice, including incoming reference snapshots, a wide configuration read, null
configuration failure before mutation, delayed casts and initialization failure.
All generated source compiles and full original metadata remains unchanged.
Six same-Code Signature controls and eight independent wrong programs are
rejected or detected without changing the original runtime corpus or observer.

`test_mobile_android_flat_pair_transfer.py --private-transfer` reuses the public
two-formal carrier proof for one owned private static input transfer. Both
Object-bounded method formals must occupy the complete input and invariant result
slots in the same order; each input slot may be invariant or covariant. A raw
same-class wrapper retains its exact private call. Foreign callers and references
through another owner remain rejected by the ordinary private invocation check.
Source and immutable carrier-JAR profiles each compare 192 fresh JVM cases twice,
compile every generated source, and preserve all original entries, access flags,
Signatures and Exceptions. Six same-Code Signature controls reject lost, swapped,
contravariant or variant slots; eight independent wrong programs check getter
effects, mutation, partial snapshots, actual results and exception identity.

`test_mobile_android_allocated_binary_callback.py` retains a fresh raw
`Callback<T,R>` allocated before a superclass call receiving closed
`Callback<Object,Token>` arguments. The common interface graph validator permits
Object-bounded formals forwarded invariantly to known parents, including a
two-formal callback forwarding only its R slot to `Functor<R>`. The original
unary graph profile keeps its existing bound rules. Allocation and initializer
provenance, complete constructor arguments, unique fixed-arity selection and
original metadata remain required. Source and immutable contract-JAR profiles
each compare 192 fresh JVM cases twice and compile every generated source.
Observers cover initialization failures, partial parent/child fields, long
arguments and delayed callback-result checks. Six same-Code graph controls and
eight independent wrong programs must fail. No callback result or generic
implementation Signature is inferred from the raw object.

`test_mobile_android_seeded_void_carriers.py --private-helper` applies the
existing seeded void-consumer proof to an owned private static body. A direct
Object-bounded seed and complete recursive callback and closed sibling input
trees retain their original scope and erasures. Private invocation still requires
the exact declaring owner and caller. Source and immutable input-interface JAR
profiles each compare sixty fresh JVM cases twice, compile every generated Java
source, and preserve all method entries, metadata and Exceptions. Signature
controls reject missing seed/callback bindings and extra formals. Eight wrong
programs check repeated or bypassed calls, callback/task order, long flags,
partial saved results and original exception identity.

`test_mobile_android_paired_seed_factories.py` checks two independent direct
Object-bounded inputs and an invariant binary result. The original provider
mutates each input in order, then returns an independently supplied Map. Source
and frozen-provider runs retain five classes/thirteen methods and two classes/six
methods; each compares 276 fresh JVM cases with two attempts. Input identity,
aliased inputs, failure before either mutation, nulls, array bounds, independent
key/value casts, result mutability and initialization failure remain observable.
Original descriptors, Signatures, metadata, Exceptions and helper accounting are
checked after Java 8 compilation. Seven Signature mutations keep Code unchanged
and are rejected. Independent bypass, repetition and swapped input calls all
fail the unchanged observer. Native cases also retain permutations of method
formals in either input or result order without changing the original call.

`test_mobile_android_array_seed_factories.py` checks a single generic array input
and an invariant unary result. Source and frozen-provider profiles each compare
136 fresh JVM cases with two attempts, retaining five classes/twelve methods and
two classes/five methods respectively. The original provider mutates the actual
array and returns an independently supplied list. Observers check array identity,
covariant stores, mutation timing, null and empty arrays, result aliases,
mutability, late element casts and first/repeated initialization failure.
Original metadata, Signatures, Exceptions, inventories and helpers remain exact
after Java 8 compilation. Five Signature changes keep Code unchanged and reject.
Independently bypassed, repeated and copied-array calls fail the unchanged oracle.
Native cases additionally cover declaration order, inherited facades, range
calls, extra component dimensions and unsupported dispatch or owner contracts.

`test_mobile_android_enum_array_factories.py` checks recursive Enum bounds on
the method and returned carrier. Source and frozen-provider/carrier profiles
each compare 160 fresh JVM cases with two attempts, retaining six classes/
thirteen methods and two classes/five methods respectively. Standard JVM enum
values exercise raw and covariant enum arrays, exact array identity and stores,
nulls, bounds, wrong-enum stores, pre-store Enum casts and later result casts.
The provider returns an independently supplied carrier with observable aliases
and mutation. Original inventories, metadata, Signatures, Exceptions and helper
accounting remain checked after Java 8 compilation. Six same-Code contract
changes reject; bypassed, repeated and copied-array implementations fail the
unchanged observer. Native cases retain all three recursive scopes, class and
interface carriers, and reject missing or weakened platform declarations.

`test_mobile_android_nested_pair_factories.py` retains two independent covariant
input carriers and an invariant list of invariant binary carriers. Source and
frozen-provider profiles each compare 372 fresh JVM cases with two attempts,
retaining five classes/thirteen methods and two classes/six methods. Immutable
caller-owned iterables record iterator creation, reads and exact failures; the
provider mutates each supplied element before returning an opaque nested result.
Checks include input aliases, both result identities and mutability, empty or
null inputs/results, independent key/value casts and initialization failure.
Full metadata, Signatures, Exceptions, inventory and helper accounting survive
Java 8 recompilation. Eight same-Code Signature mutations reject; independently
bypassed, repeated or swapped calls fail the unchanged observer. Native cases
also cover invariant inputs, formal permutations and every nominal layer's
bounds, arity and accessibility.

`test_mobile_android_nested_pair_consumers.py` exercises the inverse relation:
a covariant carrier of covariant pairs becomes an invariant binary result.
Source and frozen-provider profiles each compare 384 fresh JVM cases with two
attempts, preserving five classes/thirteen methods and two classes/six methods.
An immutable iterable observer retains iterator timing and exact failures;
provider observations retain both input layers, ordered mutations of both
elements, aliased elements, nulls, bounds and actual result identity. Returned
maps may be unrelated, immutable or the original input pair. Separate checked
key/value callers retain their original cast timing. Original metadata,
Signatures, Exceptions, inventories and helpers are checked after Java 8
compilation. Eight same-Code contract mutations reject; bypassed, repeated and
eagerly iterated replacements fail the unchanged observer. Native cases cover
all combinations of invariant/covariant input slots and result permutations.

`test_mobile_android_mapped_carrier_invocations.py` covers a covariant input
carrier, a callback consuming its formal and returning a different formal, and
an invariant result carrier. Source and frozen-provider profiles each compare
52 fresh JVM cases with two attempts, using direct, immutable, reflected and
method-handle callers. The original callback consumes a mutable array and
returns a distinct reference type. Observers check input and output identity,
field alias changes, null carriers/values/results, callback casts, exact original
failures, wide bits and first/repeated initialization failures. Complete runtime
inventory, metadata, Exceptions and helper accounting are checked after compiling
the source. Metadata-only changes to the producer, result formal or formal count
must reject without changing Code, and an independent double callback must fail
the unchanged observer. Native cases additionally cover virtual/interface
bindings, reversed parameter/class order and malformed or crossed type relations.

`test_mobile_android_unwritten_static_finals.py` compiles a class with twelve
unwritten static fields, then changes only their final access bits. Code,
descriptors, Signatures and other attributes remain byte-for-byte intact. Source
and frozen-Trace profiles each compare thirty fresh JVM cases with two attempts
across the compiler original, encoded input and recompiled recovery. Direct,
immutable, reflected, method-handle and newly compiled callers check all scalar
zero bits, null references/arrays, reentrant reads before and after an explicit
sibling assignment, inherited field initialization, exact original failures and
failed-class reuse. A fresh caller must retain identical getstatic bytecode.
Complete runtime entries, field metadata (including ConstantValue absence),
Exceptions and helper accounting are checked after recompilation. A separately
compiled constant-inlining variant must fail the unchanged initialization
observer. Native cases retain missing instance-write guards and reject partial,
foreign, aliased or unresolved stores without confusing unrelated field owners.

`test_mobile_android_mangled_interface_names.py` uses the unmodified Kotlin
compiler to produce a value-class return whose interface and implementation
methods have a JVM-only name. A branch interface, generic superclass and child
override retain that name through D8. A separate final class owns an original
final method bearing the same JVM name without implementing the interface.
The recovery report maps all four declarations to one collision-free Java name,
records their exact interface or class roots and discloses binary-ABI changes.
Ninety-eight fresh JVM cases, each with two attempts, compare ordinary wrappers,
immutable callers, reflection and method handles. They check interface,
inherited and superclass dispatch, mutable alias identity, nulls and exact
original exceptions. Full class and method metadata, compiler annotations,
Exceptions and helpers are checked after recompilation; only declared name
projections may differ from DEX metadata. An independent SDK annotation dump
confirms D8 removed CLASS-retained annotations before recovery; their complete
original payloads are recorded separately. A frozen external interface must reject recovery, and
independently compiled overrides that skip superclass effects or repeat final
method effects must fail the unchanged observers. Native cases also check an
isolated final class, package access, input order and incomplete family guards.
A counted-work regression compares 128 legal and mangled methods with 1,024
unrelated dependency classes to reject repeated whole-input metadata scans.
Original Kotlin compilation uses JDK 21; recovered
constructor prefixes use the reported Java release with JDK 25 or newer.

The `--enclosing` profile adds an unchanged Kotlin-generated anonymous object
inside the generic implementation. A complete source-owned virtual family and
its anonymous class share the disclosed source method name; external or named
local EnclosingMethod contracts remain rejected. The original and source
EnclosingMethod are both reported. Ninety-eight fresh JVM cases also check
that reflection resolves the original declaring method under its mapped name.
Independent class-file inventory verifies all eight classes and seventeen
methods, retained Kotlin annotations, local access flags and javac's added
outer constructor parameter. A third compiled counterexample moves only the
anonymous object's enclosing method while retaining effects; it must fail the
frozen observer.

`test_mobile_android_closed_array_super_constructor.py` checks a closed array
parameter forwarded to a generic parent's original array constructor, alongside
an unrelated generic List overload. Source and frozen-provider profiles each
compare 224 fresh JVM cases with two attempts. The caller retains its absent
Signature, captured fields set before super, constant and wide arguments, and
original class/method metadata. The observer checks the array's exact identity,
CharSequence/String array stores, null/bounds/type failures, partial construction,
initialization order and exception identity through immutable callers, reflection
and method handles. Java 25 input and a D8-compatible version-only encoding run
against the same oracle before recovery. Independently compiled wrong-overload,
delayed-capture, copied-array and swapped-scalar variants must all fail. Native
cases also reject changed scopes, incompatible arrays, competing array/root
reference overloads, missing declarations, method formals and variable arity.

`test_mobile_android_captured_generic_constructor.py` checks a constructor that
writes its captured field before forwarding a constant, an erased generic
consumer and a wide incoming value to its original superclass. Source and frozen
provider profiles each compare 36 fresh JVM cases with two attempts, including
callback reads of the captured field, nulls, original exceptions and first/repeated
initialization failure. Every original runtime entry, complete Signature and
helper is checked after recompilation. The original Java 25 compiler output and
the D8-compatible input execute the same immutable observer: only the two class
version bytes change for D8, with every Code and attribute byte preserved.
Separately compiled wrong-overload and late-field-store versions must fail the
observer. Native cases also reject missing origins, changed variance, broken
wide arguments, receiver clobbers and unproved overloads. The Kotlin mangled-name
fixture additionally verifies an exact source superclass type-formal binding.

`test_mobile_android_erased_constructor_delegations.py` uses the unmodified
Kotlin compiler to generate a raw public synthetic access constructor forwarding
to a private constructor with a closed function-parameter Signature. An independent
SDK dump checks the original two-instruction body. Forty fresh JVM cases with
two attempts compare factory calls, reflection and method handles, including an
ignored nonnull marker, null arguments, callback cast/unbox failures, exact wide
bits and original exception identities. Full inventories and metadata are checked
after compiling the recovered source; only the disclosed synthetic constructor
flag may change. D8 annotation losses are independently witnessed before recovery.
An independently compiled repeated callback must fail the immutable observer.

`test_mobile_android_delegation_continuations.py` checks constant and computed
arguments before `this(...)` followed by field reads and inherited virtual calls.
Two constructor depths retain the primary constructor's final writes. Source and
frozen-provider profiles compare 588 fresh JVM cases with two attempts, retaining
five classes/twenty-three methods and three classes/seventeen methods. Exact own-class
private helpers and own-class or same-package protected/package helpers retain
their visibility and run once before delegation. A separate provider initializer
changes the observed argument and can fail on the first and repeated invocation,
including when that provider is a frozen dependency. Immutable callers,
reflection and method handles observe partially initialized finals, exact wide
values, null inputs, retained parameters, callback exceptions and first/repeated
initialization failures. D8 independently witnesses both nonempty linear prefixes
and continuations. Full inventories, metadata, Exceptions and helpers are checked
after Java 25 compilation. Wrong constants, repeated argument calls, reordered
observations, changed virtual arguments and lost live inputs must all fail the
unchanged observer. Native cases also reject final rewrites, cycles, missing
primary writes, foreign private or cross-package nonpublic calls, declaring-class
aliases, changed static dispatch, broken result
carriers and unproved continuation control flow.

`test_mobile_android_carrier_target_names.py` checks a final method-local
carrier implementation whose original primitive target has a JVM-only name.
The already verified carrier bridge supplies its complete scoped contract;
the spelling plan still rejects inherited name conflicts, unknown ancestry,
nonfinal owners and external enclosing-method references to the changed name. Calls
retain their exact declaring method. Source and immutable-carrier-JAR profiles
compare interface calls, a frozen caller, reflection and method handles, including
wide edge values, boxing identity and every conversion/target failure. The owned
input encoder records final anonymous-class flags, the target UTF8 name, interface
Signature and bridge flags, proving every Code byte unchanged before executing
the actual input.
Recompiled inventory and metadata allow only the disclosed name, raw interface,
bridge flags and generated helpers. Coverage remains `partial` because Java
cannot reproduce the original JVM method name or binary callers of that name.

The `--enclosing` carrier-name profile additionally allocates an anonymous class
inside that original primitive target. Only source-owned anonymous declarations
may follow the verified carrier target's spelling; external EnclosingMethod
references remain rejected. Each class binding retains its original enclosing
method and reports `source_enclosing_method` with the new name and unchanged
parameters/result. Reflection checks that exact method plus the captured receiver
identity. Existing anonymous-to-local constructor and synthetic-field differences
are verified separately and remain disclosed; source requires Java 25 for the
original pre-super capture write.

`test_mobile_android_delegation_postludes.py --loop` checks a closed constructor
chain whose initialized continuation contains a loop. Its branches must remain
after the single verified delegation; guarded prefixes still require a pure
return tail. The shared delegation proof rejects prefix re-entry, missing targets,
result-entry branches, handlers and final rewrites. The body verifier retains
instruction semantics, register joins and reachability. Source and immutable
trace-JAR profiles each compare 72 fresh JVM cases with two attempts through
four call routes. They observe every loop update, integer overflow, partial
initialization, final/reference identity and exact exception identity. Independent
compiled short-loop and reordered-write variants must fail the frozen observer.
All original runtime entries, metadata, Exceptions and generated helpers are
accounted for after recompiling the recovered Java.

`test_mobile_android_allocation_field_aliases.py` preserves an original field
read, reference cast and alias while constructor-argument preparation reuses
registers for scalar values. Source and frozen-provider profiles each compare
32 fresh JVM cases with two attempts. The SDK dump independently verifies one
field read after allocation, its cast and reference/scalar/wide copies before
the range constructor call. Quiet NaN payloads, negative zero, wide bits, nulls,
cast failures and first/repeated initialization failures are checked. A constructor
changes the original field, so post-construction alias checks detect an extra
read. Full runtime inventories, metadata and helpers are checked after recompiling.
Independently compiled hoisted-read, late-reread and numeric-float-copy variants
must all fail the immutable observer. Native cases additionally cover instance
fields, exact-zero reference views and malformed copy/initialization boundaries.

`test_mobile_android_allocation_field_chains.py` checks consecutive reference
field reads followed by an int field argument to a same-package protected
constructor. Source and frozen-provider profiles retain five classes/ten methods
and four classes/eight methods, comparing 144 fresh JVM cases with two attempts.
The SDK dump witnesses the leaf read overwriting its receiver register. Immutable
callers, reflection and method handles check initialization before field reads,
null receivers, int boundaries, partial construction and live reference, int and
intermediate-link identities after the constructor changes the original fields.
First and repeated initialization failures retain their original exception
behavior. Full inventory, metadata, Exceptions and helpers are checked after
Java 8 compilation. Independently compiled hoisted reference/int reads and late
reference/int/link reads must fail the unchanged observer. Native cases cover
range calls, integer leaves, same-class private constructors and frozen
declarations while rejecting foreign access, clobbers and malformed regions.

Its `--boolean-fields` profile authors boolean fields, constructor parameters
and observers before unmodified javac/D8 compilation. Both source and frozen
provider variants repeat all 144 fresh JVM cases, including two attempts per
case, with the same complete metadata and helper inventory. They distinguish
true/false values before and after target initialization, retained boolean
values after constructor mutation, null receivers and original failures. The
same five independent hoisted/late-read counterexamples must be detected.
Native cases also cover a boolean leaf overwriting its reference receiver,
boolean return values, range calls and known dependencies, while rejecting
opcode/type disagreement, static/instance mismatch and uninitialized receivers.

`test_mobile_android_allocation_array_lengths.py` retains an array length between
allocation and constructor invocation. Direct, field-backed, retained-register
and nested-allocation variants each use immutable callers, reflection and method
handles. Source and frozen-provider profiles retain five classes/twelve methods
and three classes/eight methods, comparing 160 fresh JVM cases with two attempts.
Outer initialization replaces an array before its length is read; inner
initialization and the constructor replace it afterward. The oracle checks
null and empty arrays, escaped partially constructed objects, first/repeated
initialization failures and original exception identity. Original inventory,
metadata, Exceptions and helpers are checked after Java 8 compilation. Four
independent hoisted, repeated and delayed field/length implementations fail the
unchanged observer. Native cases also cover primitive, reference and nested
arrays, register aliasing, range calls and otherwise unused length effects.

`test_mobile_android_allocation_array_arguments.py` preserves array allocation,
filled-array construction and stores between an object allocation and its
constructor. Source and frozen-provider profiles retain four classes/eleven
methods and three classes/nine methods, comparing 204 fresh JVM cases with two
attempts. Direct calls, immutable callers, reflection and method handles check
negative lengths, nulls, bounds, covariant stores, partial construction, array
identity, live aliases and first/repeated initialization failures. The SDK dump
independently witnesses the original array and store ordering. Full inventories,
metadata, Exceptions and helpers are checked after Java 14 compilation, which
keeps stores inside constructor arguments using a switch expression. Independently
compiled hoisted reads/allocations, repeated reads, delayed stores and copied
aliases must fail the unchanged observer. Native cases cover all element types,
multidimensional arrays, incoming stores, range calls and malformed regions.

`test_mobile_android_nested_final_branches.py` checks a constructor that selects
one final reference across three nested branches, writes common scalar and wide
finals, then conditionally allocates a separate object. Source and frozen-provider
profiles compare 432 fresh JVM cases with two attempts, retaining seven classes
and fourteen methods, or two classes and four methods. Immutable direct callers,
reflection and method handles observe escaped `this` at every stage. The oracle
checks original store timing, exact wide bits, object identities, virtual effects,
nulls, partial construction, exception identity and repeated initialization
failures. Full inventory, metadata, Exceptions and helper accounting are checked
after Java 8 compilation. Independently compiled delayed final stores, repeated
provider calls, early common stores and reads hoisted before allocation all fail
the unchanged observer.

`test_mobile_android_allocation_call_aliases.py` checks a static constructor
argument call whose result overwrites its field-read input, both with and
without a later reference copy. The SDK disassembly must independently witness
the original field/call/result register reuse. Source and frozen-provider
profiles each compare 80 fresh JVM cases with two attempts. Target and provider
initializers mutate fields, and the constructor replaces the original input;
immutable callers check original argument/result identities, booleans, nulls,
mutable arrays, casts, exception identity and repeated initialization failures.
Full runtime entries, metadata, Exceptions and helper inventories are checked
after recompilation. Independently compiled hoisted-call, repeated-call and
late-reread variants must fail the unchanged observer. Native cases additionally
check plain incoming aliases, reference casts, wide arguments, range calls,
input order and malformed value/control-flow boundaries.
The `--call-access package`, `protected` and `private` profiles compile separate
original source witnesses before D8. The first two keep nonpublic provider calls
within their runtime package; the last retains an original private wrapper in the
caller class. Each profile executes the same 80 cases in source and frozen-provider
modes, preserving original access flags and accounting for the private wrapper's
additional method. The unchanged oracle detects hoisted or repeated calls and late
alias reads. Native nested-allocation cases additionally exercise integer/reference
results, void effects, exact declaring owners, dependency declarations and range
calls, while rejecting foreign access, declaring-class aliases and producer drift.

`test_mobile_android_allocation_instance_calls.py` compiles package and protected
instance methods from original Java/D8 sources. Same-class and same-package
construction paths retain virtual overrides, incoming receiver identity and
allocation before the call. Source and frozen-provider profiles each run 96 fresh
JVM cases with two attempts, through direct, immutable, reflected and method-handle
callers. They check mutable results, null inputs/receivers, ordered call and
constructor failures, and first/repeated initialization failure. All five original
classes and eleven methods are inventoried; frozen-provider profiles recover two
classes and four methods. Metadata, Exceptions and generated helpers remain
accounted for. A Code-preserving private-method mutation must be rejected, while
separately compiled hoisted calls and lost overrides must fail the unchanged
observer. Native cases retain final methods, exact owners and range calls, and
reject foreign package/protected access and inherited declaring-class aliases.

The static allocation-call oracle also compiles package and protected providers
with `--call-access`, preserving the original 32 cases, metadata and foreign
private-access counterexample in source and frozen-provider modes. The conditional
allocation oracle's `--constructor-access` profiles exercise package and protected
constructors across its 76 cases; `--dependency-target` also freezes the target
declaration. Conditional getters, repeated calls, allocation timing, full runtime
inventories and access flags remain unchanged. Both paths consume the same exact
member-access proof as nested allocation and constructor prefixes.

`test_mobile_android_allocation_super_calls.py` preserves reference and int
results of original nullary superclass calls inside allocation arguments.
Source and frozen-parent/target profiles retain six classes/sixteen methods and
three classes/ten methods, comparing 208 fresh JVM cases with two attempts. An
intermediate parent inherits both methods, while the caller and a deeper child
override them. The SDK disassembly witnesses exact superclass dispatch after
allocation; immutable callers, reflection and method handles verify parent-body
effects, actual receiver identity, live results, partial construction, nulls, int
boundaries and first/repeated initialization failures. Full metadata, Exceptions,
inventories and helpers are checked after Java 8 compilation. Independently
compiled virtual, hoisted, repeated and late-result substitutions must fail the
unchanged observer.

`test_mobile_android_synthetic_instance_names.py` uses unmodified javac and D8
to produce a package-visible synthetic instance lambda implementation with an
unrepresentable Java name. The independent SDK dump verifies its virtual entry,
original callback/finally body and generated adapter forwarding. Source and
frozen-trace profiles retain four classes/ten methods and three classes/eight
methods, comparing 80 fresh JVM cases with two attempts. Immutable callers,
reflection and method handles check captured receiver/callback identity, mutable
state, subclass cleanup dispatch, nulls and original exceptions, including a
cleanup failure replacing a callback failure. Original DEX entries, the reported
method-name projection, helper inventories and unprojected metadata are
checked after recompilation. The evidence separately records javac-to-D8 helper
renaming/access and Java's inability to reproduce synthetic adapter class,
capture-field and constructor flags; coverage remains `partial`. Independently
compiled skipped/repeated callbacks, omitted cleanup, foreign receivers and
hoisted callback effects must fail the unchanged observer. Native cases also
exercise same-package override families, arbitrary illegal names, name
collisions, input order, external members, unknown ancestry and invalid callers.


The Java monitor runner's `--shared-return` profile uses unmodified javac/D8
output whose three-instruction catch-all release/rethrow precedes a common
return. The exception table identifies cleanup independently of method layout.
Source and frozen-payload profiles each retain the complete original inventory,
Signatures, fields, Exceptions and helper accounting. In addition to the existing
6,000 concurrent updates and reentrancy/release checks, 64 direct, frozen,
reflected and method-handle cases cover skipped acquisition, null callbacks and
locks, original callback/body exception identity and mutable state. An
independently compiled body that acquires a lock on the skipped branch must fail
the unchanged observer, as must an unsynchronized body. Native cases also retain
post-cleanup calls, conditional continuations and reuse of the released lock
register, while rejecting normal entry into cleanup, re-entry, release bypasses,
changed rethrow values and unproven trailing blocks.


The static-name runner's `--ordinary-static` profile compiles its Kotlin source
without `@JvmSynthetic`. Unmodified javac/Kotlin/D8 originals supply three public
static final names, including overloaded reference/long results and a void
operation. Source and frozen-box profiles each compare 60 fresh JVM cases,
complete original method inventories, descriptors, Signatures, access flags,
fields, runtime annotations, Exceptions and reported helpers. Initialization,
reference identity, wide values, ordered failures and void effects must match.
Java names and their ABI/reflection differences are disclosed; nonsynthetic
flags remain unchanged. A same-body private access mutation still fails name
planning, and an independently compiled wrong void effect fails the observer.
Native cases cover inherited symbolic owners, reserved-name collisions, class
order, final flags and generic declaring classes. The unchanged generic-call
proofs continue to govern every resolved invocation.


Allocation argument programs share the ordinary scalar conversion and unary
opcode tables. Floating add, subtract, multiply, divide and remainder consume
one exact type/symbol receipt for both normal and `/2addr` forms; integer
division does not gain admission from that receipt. Immutable value nodes retain
raw floating carriers, original field reads and class initialization order.
`test_mobile_android_allocation_scalar_conversions.py --floating` compares 120
fresh JVM cases with two attempts in source and frozen-provider profiles, with
ten floating opcode families and seven independently compiled counterexamples.

`test_mobile_android_allocation_array_payload.py` preserves all eight primitive
array payload families inside ordinary, protected and synchronized constructor
arguments. The ordinary body and argument graph share the complete length check
and element decoder, so an undersized array fails before its first store. Source
and frozen-target profiles compare 72 fresh JVM cases with two attempts, full
runtime inventories, metadata and Exceptions, raw negative zero, fresh array
identity, ordered callbacks and first/repeated initialization failures. Four
independently compiled counterexamples check changed payloads, initialization
order, monitor ownership and caught exception identity. Nonexecuted alignment
nops after terminal monitor cleanup consume the same empty-instruction shape
check as ordinary unreachable code; executable entries remain rejected.


A public concrete superclass method can root a complete source virtual naming
family, including a concrete body declared by an abstract class. The existing
family verifier retains exact descriptors, scopes, original identities and one
shared fresh spelling across known overrides. Interface roots retain precedence;
unknown ancestry, bridge adaptations, enclosing-method metadata and unchanged
SDK members still prevent this projection. `test_mobile_android_concrete_virtual_names.py`
uses unmodified Kotlin/D8 output with an inherited intermediate class and two
override levels. Source and frozen-initializer profiles compare 72 fresh JVM
cases with two attempts through ordinary, immutable, reflected and method-handle
calls, including lexical superclass dispatch, mutable results and original
failure identity. Class/method metadata remains intact except the reported
source names and ten exact compiler CLASS annotation losses; coverage stays
`partial`. An unchanged SDK method root must fail explicitly, and three
independently compiled effect/result/callback counterexamples must fail the
same observer.


A descriptor-only fixed-arity leading constructor call can separate a generic
reference sibling at a primitive argument position before boxing. The selected
target must have no source or original method Signature; this rule cannot waive
its generic binding. Primitive arguments keep explicit selected-descriptor
casts, including source-only widening conversions discarded by DEX, so narrower
primitive overloads cannot change dispatch. Reference targets and selected
variable-arity/generic constructors retain the existing separate proof gates.
`test_mobile_android_primitive_constructor_overloads.py` compares 168 fresh JVM
cases with two attempts in source and frozen-parent profiles, retaining complete
Signatures, fields, Exceptions and inventories. Generic copy, method-formal and
variable-arity siblings remain available without being selected. The byte
forwarder profiles add a narrower short overload and compare 252 cases; an
independently compiled omitted widening cast must fail the unchanged observer.
The oracles also compare argument/class initialization order, partial object
publication, numeric edges and original failure identity through direct,
immutable, reflected and method-handle constructors.


A descriptor-only forwarding constructor in a known generic source class may
retain a raw incoming carrier while calling its direct superclass's scoped
carrier constructor. One shared body receipt requires the original two-operation
forward/return body, unchanged declaration-order incoming words, exact erasures,
no handlers or method formals, and fixed-arity overload selection. Class-scoped
leaves must resolve to the caller's retained Object-bounded formals; known
public carrier declarations retain their full bounds and argument arities.
Invariant and upper-wildcard arguments use those declared scopes without
inventing a constructor Signature. Anonymous closed forwarding keeps its
existing separate scope gate. `test_mobile_android_scoped_raw_constructors.py`
compares 84 fresh JVM cases with two attempts in source and frozen-parent
profiles, preserving all metadata, original raw parameter reflection, defaults,
mutable alias effects, partial construction and failure identity. Four compiled
counterexamples must fail the unchanged observer.


A static method's sole Object-bounded formal can be supplied entirely by a
fully checked input carrier when the result is primitive. The shared nested
carrier verifier retains every public nominal declaration, bound, arity and
scoped occurrence; direct seeds, unknown bounds and contravariant occurrences
do not gain admission from this rule. Static scope cannot borrow an owner's
class formals, even when the owner itself is generic. Source package helpers
retain their exact declaring reference and runtime package checks, while SDK
entries require public access. `test_mobile_android_unseeded_scalar_carrier.py`
compares 108 fresh JVM cases with two attempts through ordinary, immutable,
reflected and method-handle callers, retaining complete original inventories,
Signatures, fields, Exceptions and helper accounting. Its source, frozen-helper,
package-helper and frozen-callback profiles verify callback identities and
mutations, monitors, nulls, late casts and first/repeated initialization errors.
Four independently compiled result/callback/input/monitor counterexamples must
fail the unchanged observer.


A public static array-valued generic operation may retain one Object-bounded
method formal across its one-dimensional input and result arrays. Every array
slot must name that same declared formal; extra parameters may be primitive.
The shared array-formal receipt preserves original calls, actual array objects,
provider allocation and later checked casts without assuming which input is
returned. `test_mobile_android_generic_array_transfers.py` compares 52 fresh JVM
cases with two attempts in source and frozen-provider profiles, retaining full
inventories, Signatures, fields and Exceptions. The independent array-copy oracle
checks overlap, actual covariant components, nulls, bounds errors and partial
stores before ArrayStoreException. Four compiled result/copy/identity/offset
counterexamples must fail the same observer.


The generic array transfer receipt also covers package-visible source helpers,
including a package-visible declaring class. It retains exact declaring-owner
and original runtime-package checks for every call; foreign, inherited-name,
private, native, abstract and unchanged SDK package entries remain rejected.
The array transfer runner's `--package-provider` profiles preserve package
access and compare the same 52 cases in source and frozen-trace modes. No name,
visibility, method Signature or array object is adapted.


Allocation argument graphs retain array element reads as immutable ordered
producers, including reads that reuse their array register. Ordinary array
verification owns the element descriptor, load opcode, width and frame; graph
locals use the same floating/boolean register-storage conversion before scalar
or reference aliases copy them. Original null and bounds failures remain before
later calls and construction. `test_mobile_android_allocation_array_reads.py`
compares 120 fresh JVM cases with two attempts in source and frozen-constructor
profiles, with all nine element families, raw floating payloads, defaults,
post-read mutations, class initialization, protected exits and monitor ownership.
Metadata, Exceptions and inventories stay unchanged; four independently compiled
identity/bit/index/monitor counterexamples must fail the original observer.


Allocation graphs use the ordinary integer add/subtract receipt for register,
two-address and signed literal forms. Inputs are captured before destination
writes, preserving Java/Dalvik wrapping arithmetic and getter order. The integer
argument runner's `--calls` profiles compare 72 fresh JVM cases with two attempts
in source and frozen-constructor modes, including initialization, getter and
constructor failures; unchanged metadata and three compiled counterexamples are
checked alongside the original profiles. Uniform protected graphs are supported;
changed cleanup regions and unproved operands remain rejected.


Up to sixteen disjoint sequential monitor acquisitions can each own their
reachable locked body, normal releases and exact catch-all release/rethrow.
The unlocked control-flow graph treats each verified region as one transition;
bypasses and callbacks between regions retain their original lock state. Shared
cleanup, nested/overlapping regions, changed lock registers, uncovered effects
and entries that bypass acquisition remain rejected. The sequential monitor
runner compares 208 fresh JVM cases with two attempts per source/frozen-callback
profile, covering distinct/shared locks, external reentrancy, early exits,
skipped acquisitions, null locks and original callback failures. Five compiled
counterexamples must fail; metadata, Exceptions, inventory and input bytes stay
unchanged.


Released continuations may loop back through acquisition. Removing acquisition
nodes from the unlocked CFG must leave an acyclic graph, so every cycle retains
a new acquisition and a paired release. Direct locked-body entries and unlocked
cycles remain rejected. The sequential monitor runner's `--loop` profiles compare
208 fresh JVM cases with two attempts and repeated iterations in source and
frozen-callback modes. Six independently compiled counterexamples include an
omitted iteration; reentrancy, null acquisition, skipped/early exits, original
failure identities and complete metadata remain unchanged.


Referenced public static void array writers may retain one Object-bounded
method formal in exact one-dimensional array parameters and direct element
seeds, with primitive auxiliaries. The operation receipt keeps every argument's
runtime erasure and the provider's original stores, callbacks and failures;
it adds no element cast or array copy. Native/abstract, varargs, extra formals,
changed bounds, unproved ranks and projected contracts remain excluded.
`test_mobile_android_generic_void_array_writers.py` compares 52 fresh JVM cases
with two attempts per source/frozen-provider profile, preserving array/value
identity, null/range/covariant store failures, first/repeated initialization,
callback exception identity and full metadata. Six compiled counterexamples
include copied arrays and an early element cast.


Superclass constructor receipts retain incoming reference origins through fresh
local upcasts. Each complete invariant, Object-bounded generic superclass edge
must have an independent declaration and no projection; the shared bound proof
composes the ancestor arguments into the selected constructor's full contract.
The existing fixed-arity overload proof and exact cast keep the original target.
Unproved/downward casts, changed scopes/arguments, lost origins, non-Object
bounds and method-formal or variable-arity competitors remain rejected.
The constructor upcast runner compares 48 fresh JVM cases with two attempts per
source/frozen-parent profile, including a package-scoped child, prefix checks,
initializer failures, overload selection and partially captured field defaults.
Five compiled counterexamples must fail; metadata and inventories remain exact.
Support classes are compiled unchanged for Java 8, while only the flexible
constructor uses Java 25. Assigned CI shards fetch the pinned official R8 9.5.23
D8 artifact and verify its SHA-256; existing profiles use their original SDK D8.


The same exact array-operation receipt retains unchanged primitive results as
well as void. Its one Object-bounded component, original argument erasures and
complete provider call remain authoritative; each original result register uses
the ordinary declared primitive carrier. Native tests cover all eight return
families. The array writer runner's `--scalar` profiles compare 52 fresh JVM
cases with two attempts in source/frozen-provider modes, observing a live integer
result alongside mutations, alias identity and ordered failures. Seven compiled
counterexamples include a replaced result; inventories and metadata stay exact.


A raw synthetic default-mask constructor in an independent public final generic
class can consume the shared same-owner delegation receipt. Its original value
parameter prefix, integer mask count and Kotlin marker descriptor stay exact;
its class formals and every known nested carrier must remain Object-bounded and
unprojected. The selected fixed-arity constructor's complete class-scoped
contract supplies operation-local casts, without adding a caller Signature or
adapting callback arguments. The original branch program retains default values,
static field initialization and object identity. Unsupported scopes, carriers,
mask/marker layouts and competing inferred/variable-arity targets still fail.
The authentic Kotlin runner compares 128 fresh JVM cases with two attempts per
source/frozen-default-provider profile and four compiled counterexamples.
Original inventories and metadata remain exact except the existing disclosed
synthetic constructor flag projection. D8 removal of four exact CLASS annotation
sites is independently verified and recorded; runtime annotations remain exact.

A final raw local class can retain a fixed covariant reference result while its
Object-erased parameter refers to an Object-bounded enclosing class formal.
The exact original three-operation bridge, direct raw interface slot and known
superclass scope authorize one disclosed source parameter erasure; input DEX and
external dependency declarations consume the same contract;
additional ancestor slots require separate complete inheritance proofs. A static
self allocation uses the existing shared member declaration receipt, keeping
one original singleton and reporting its changed source ownership and identity.
The authentic Kotlin runner checks default and class lambda generation, 32 fresh
JVM cases with two attempts in each profile, frozen interface callers, reflection,
method handles, callback failures, null inputs, singleton identity and Lambda
arity. Three independently compiled counterexamples must fail. Original methods,
runtime annotations and other metadata stay exact except the disclosed class
relocation, constructor synthetic flag and single parameter Signature projection;
two precise D8 CLASS annotation loss sites are independently recorded.

The input-runtime variant includes unmodified Kotlin Function, Function1,
FunctionBase and Lambda class bytes in DEX and verifies their rebuilt method
inventory and metadata too. It repeats all 32 JVM scenarios and three compiled
counterexamples; its additional CLASS annotation loss is limited to Lambda
`toString` and is independently recorded.

Constructor argument value graphs also retain 32-bit integer multiplication in
register, two-address and signed literal forms. Field-fed total arithmetic
selects the same graph as effectful calls, so class initialization and each
field read precede the corresponding argument calculation. The source and frozen
constructor profiles each compare 64 fresh JVM cases with two attempts, including
wrapping overflow, class initialization failures, constructor failures and three
compiled counterexamples. Dividing integer preparations still require a separate
throwing-operation proof.

`test_mobile_android_anchored_void_carrier.py` verifies an original generic
static void call whose invariant unary input binds its single Object-bounded
method formal. Only that complete binding allows contravariant callbacks and
fixed parameterized arguments to share the existing erased invocation proof.
The common bounded type-tree validator still checks every nominal argument,
its arity, visibility and Object bounds; class formals, unknown types, changed
bounds and unproved variant anchors remain rejected. Original declarations and
Signatures are retained. Source and immutable-provider-JAR profiles each run
48 fresh JVM cases with two attempts through ordinary calls, a frozen caller,
reflection and method handles. They check callback order, mutable aliases,
null inputs/results and exact exception identity. Three independently compiled
variants repeat a callback, reorder a fixed callback or copy the carrier; all
must fail the frozen observer. Complete original runtime entries, metadata,
Exceptions and generated helper accounting are checked after recompilation.

`test_mobile_android_unary_void_carrier.py` checks a void consumer with one
invariant or covariant carrier of its sole Object-bounded method formal. The
existing carrier proof validates the complete nominal declaration and source
signature before allowing the original erased call. No callback is synthesized.
Source and immutable-interface-JAR profiles each compare 80 fresh JVM cases,
with two attempts, through direct wrappers, a frozen caller, reflection and
method handles. Distinct implementations verify interface override dispatch,
mutable returned aliases, null receivers/factories/results, partial field writes
and original exception identity. Independently compiled variants repeat the
factory, discard its returned object or change override effects; each must fail
the frozen observer. Complete class/method inventory, original metadata and
Exceptions remain exact after helper accounting. Native cases cover static,
virtual and interface dispatch, source/dependency declarations, range forms,
input order and rejected bounds, missing formals and variance changes.

`test_mobile_android_projected_seed_carrier.py` composes a previously proved
static reference-wrapper scope projection with an independent, directly seeded
Object-bounded method formal. The complete callback argument tree must still
validate; the wrapper's unavailable class formal becomes Object while the method
formal retains its original declaration and uses. This does not authorize
arbitrary Signature projections or a missing seed. The independently authored
classfile encoding changes only one Signature occurrence and three synthetic
flags, preserving every method Code and descriptor. Source and immutable-callback
profiles each compare 32 fresh JVM scenarios at three stages (authored, encoded,
and recovered), with two attempts per scenario. They check argument/result
identity, callback mutations, exception order and identity, null inputs, reflection
and method handles. Three compiled wrong-code variants must fail. Recompiled
inventory, all other metadata and Exceptions stay exact; reflection verifies the
single disclosed class-formal projection and unchanged method formal.

`test_mobile_android_checked_reference_return.py` retains an original checked
reference-result bridge whose same-parameter sibling returns Void. A final input
owner and exact original forwarding, result cast and return prove a separate
source name for the typed sibling; its full Signature and arbitrary body remain
unchanged. A complete known ancestor walk rejects any inherited typed slot that
would be split by this name. Existing inherited-contract validation still owns
the erased bridge parameters and result. The complete virtual name family can
include that certified retained bridge, while the typed alias keeps its separate
name. Ordinary-name, mangled-name and frozen-callback profiles each compare 40
fresh JVM scenarios at three stages, with two attempts per scenario. Normal null
returns, wide parameter boundaries, callback/result identity, ordered mutations
and failures are checked through direct calls, a frozen caller, reflection and
method handles. Three independently compiled wrong-code variants must fail.
Original inventory, Signatures and all other metadata are reconciled after the
disclosed name and bridge/synthetic flag changes. The controlled JVM name and
flag encoding preserves all original method Code bytes and descriptors.

`test_mobile_android_static_method_enclosures.py` uses unmodified Kotlin compiler
output with a mangled static entry, its synthetic default-argument helper and an
anonymous object allocated inside that helper. Source-owned anonymous locals
consume the existing exact source EnclosingMethod mapping for static name changes;
external and named local metadata still require the original declaration name.
The frozen observer compares 64 fresh JVM scenarios with two attempts before and
after recompilation, checking default versus explicit arguments, fresh object
identity, the exact reflected helper, mutable aliases, nulls, ordered effects,
partial writes and class initialization failures. Three independently compiled
wrong Kotlin programs must fail. All original definitions, runtime Kotlin
metadata and other metadata are reconciled after the disclosed source name,
synthetic flag and local class identity/InnerClass changes. D8 CLASS annotation
loss is independently checked at four exact sites.

`test_mobile_android_private_instance_names.py` verifies genuine Kotlin private
instance names on an open generic owner. Public wrappers retain exact private
direct dispatch on both parent and child receivers; a same-named public child
entry keeps a separate virtual source name. The shared independent-body name
planner reserves all original/projected names once and uses separate deterministic
static/private name counters. Same-class declaring references remain required for
private calls. Ordinary and compiler-synthetic profiles each compare 72 fresh JVM
scenarios with two attempts before and after recompilation, including reflection,
method handles, long wrapping boundaries, repeated receiver state, null receivers,
partial writes and exact exception identity. Three wrong Kotlin programs must
fail the frozen observer. Original entries, generic owner Signature, private/final
flags and runtime Kotlin metadata are reconciled after the disclosed names and
optional synthetic flag loss. D8 CLASS annotation loss is independently recorded
at the two exact named-method sites. Native cases reject foreign/facade private
references, virtual dispatch, changed wide words, unavailable metadata and
unsupported bodies; independent private/static entries cannot contaminate a
complete public virtual name family.

`test_mobile_android_bounded_captured_factory.py` exercises a genuine Java
factory with three method formals, one nominal class bound, an invariant
two-formal converter and a captured nonstatic member result. The source and
frozen-factory profiles retain eight classes/fourteen methods and seven
classes/twelve methods respectively. Each compares sixty fresh JVM scenarios
with two attempts before and after recompilation: exact callback and parent
identities, alternate returned parents, mutated vector storage, partial
constructor writes, null inputs and ordered exception identity. Three wrong
Java programs must fail the independent observer. The exact static-member,
capture-field and constructor Signature changes are reconciled; all other
class/method metadata and declared exceptions remain unchanged. The validator
consumes the existing captured-owner receipt and proves complete scoped bounds
without deriving factory behavior from generic type variables.

Allocation preparation can retain a declared result that the original DEX
caller discards. The common bounded argument graph emits the original call as
an ordered statement after allocation/class initialization, preserving its
arguments and failures without a result register or added allocation. An
actual move-result still requires its exact carrier, adjacent producer and
unchanged branch/handler boundaries. The allocation-statements runner has
`--discarded-result object`, `integer` and `wide` profiles in both source and
frozen-provider modes. Their independently compiled callers discard each
result; D8 disassembly must witness that absence and the original allocation,
call, class-literal and constructor order. The existing initialization-cycle,
exception-identity, metadata, same-code access and hoisted-call counterexamples
apply to each profile.

The carrier-target name runner also has `--separate-family` profiles. An
independently compiled wide-argument interface and open implementation share the
encoded JVM name of an unrelated final anonymous carrier target, with a distinct
return descriptor. Only exact UTF8 name entries change; Code, descriptors and
other declarations stay unchanged. The complete interface family retains one
source spelling and the certified independent target receives another. Source
and frozen-carrier profiles retain eight classes/sixteen methods and six
classes/eleven methods, reconcile all entries and metadata, and compare 36 fresh
JVM scenarios with two attempts before and after recompilation. Concrete/interface
reflection, an immutable facade with interface dispatch and method handles check
wide values, null receivers, partial writes and exception identity alongside the
original carrier oracle. A separate early-write program must fail. Enclosing
variants additionally consume the original nested EnclosingMethod receipt.

`test_mobile_android_raw_primitive_tail.py` preserves an original raw interface
and a primitive target's complete contravariant tail Signature. Independently
authored Boolean and long bodies retain the original checked unboxing and
forward the same tail and returned object. Only the exact erased entry's
bridge/synthetic flags are encoded; every Code byte and descriptor stays
unchanged. Source/provider profiles retain five classes/seven methods and three
classes/five methods. Each compares 52 fresh JVM scenarios with two attempts
across authored, encoded and recompiled stages, including frozen interface and
primitive callers, reflection, method handles, effectful Number dispatch, null
and failure order, tail identity and returned markers. Three wrong Java bodies
must fail. Raw owner/class formals, target Signature and other metadata remain
unchanged; bridge flag loss is disclosed. Native cases cover all eight primitive
conversions and a source anonymous local owner, while rejecting full tail,
conversion, register, scope and result drift.

The seeded nominal result runner additionally supports `--private-helper` and
`--private-helper --dependency-carrier`. An independently compiled public
varargs entry delegates to a private static final generic helper with the
original array and a fixed nominal return. Both source and borrowed-carrier
profiles retain every entry (seven classes/thirteen methods and six
classes/twelve methods), compare sixty fresh JVM scenarios with two attempts,
preserve private/final flags and complete Signatures, and retain initialization,
callback, array-bit and exception behavior. Three metadata changes with identical
Code and six wrong Java programs remain rejected by the independent checks.
Native cases separately exercise an invariant state carrier, contravariant
callback and direct seed, and reject foreign/facade private references, missing
bodies or seeds, generic results, changed bounds and unavailable scope.

`test_mobile_android_split_seed_factory.py` independently compiles a factory
whose invariant parent input uses one Object-bounded method formal and whose
two direct seeds and invariant result use another. Source and frozen-provider
profiles retain four classes/nine methods and three classes/seven methods,
compare 56 fresh JVM scenarios with two attempts before and after recompilation,
and reconcile all original metadata and declared exceptions. Direct/frozen
calls, reflection and method handles check parent reads, both seed identities,
constructor publication and partial final-field writes, exact exception order,
null inputs and existing/null/parent results. Swapped seeds, a replaced existing
result and an early constructor write must fail the independent observer. No
type or metadata projection is introduced.

`test_mobile_android_interface_default_access.py` uses unmodified Kotlin compiler
output from `-Xjvm-default=all-compatibility`, including the original static
same-interface default accessor and an allocated anonymous callback. Seven
classes/thirteen methods are independently inventoried. Eighty fresh JVM
scenarios with two attempts compare original and recompiled execution through
legacy and frozen callers, reflection and method handles, with base and
overriding receivers. Compatibility calls retain default dispatch, ordinary
virtual calls retain overrides, and capture/result identities, partial effects,
nulls and exact failures match. One private source helper owns the original body
and local declaration; source helper, constructor, class-name/flag and enclosing
method differences are reconciled. All other metadata stays unchanged, including
runtime Kotlin/Deprecated annotations; D8 CLASS annotation loss is checked at
eight exact compiler sites. Wrong result, effect and virtual-dispatch programs
must fail. Native cases cover reference, primitive, wide and void carriers,
exact receiver words, local ownership and scope rejection. Coverage is `partial`
because the additional helper and disclosed source metadata differ.

The mangled interface-name oracle additionally uses `--default-root` with
unmodified Kotlin `-Xjvm-default=all` output. A concrete owned interface default
roots the same complete known virtual family as an abstract declaration when
its full source contract is unchanged. Default body validation, ancestry,
external name boundaries and all ordinary member checks remain authoritative.
The original seven classes/sixteen methods (eight/seventeen with `--enclosing`)
are inventoried and reconciled, including the default's access and Signature.
Ninety-eight fresh JVM scenarios with two attempts compare original and
recompiled behavior. A frozen receiver without an override separately exercises
inherited default dispatch through original callers, reflection and method
handles, and checks ordered effects and exact exception identity. An altered
default body must fail the observer alongside the existing super, final-effect
and enclosing-method counterexamples. Only disclosed source names, generated
helpers and local metadata change; coverage remains `partial`.

The mask constructor runner's `--fixed-nominal` profiles use a public final
ordinary class with a contravariant callback to a fixed nominal type. Kotlin's
original raw synthetic mask entry remains unchanged. Source and frozen-default
profiles reconcile five classes/nine methods and three classes/five methods,
including the target's complete Signature and raw caller reflection. Each
compares 128 fresh JVM scenarios with two attempts across original and
recompiled stages. Mask selection, lazy dependency initialization, default and
incoming callback identity, partial final fields, failures and poisoned
initialization match; four independently wrong programs must fail. Native
cases reject missing synthetic/mask evidence, target formals or variable arity,
unavailable type scope, changed receiver words and checked exceptions. The only
method metadata projection is the reported synthetic flag loss.

`test_mobile_android_bidirectional_factory.py` independently compiles a factory
with two opposite contravariant/covariant callbacks and an invariant result
carrier whose second formal has a fixed nominal bound. Source and frozen
provider modes reconcile six classes/ten methods and five classes/eight methods
without type or method metadata projections. Sixty-four fresh JVM scenarios
with two attempts compare original and recompiled calls through a source
client, immutable typed caller, reflection and method handles. Both callback
identities and directions, input mutations, null callbacks/results, partial
writes and exact failure order match. Existing, alternate and null carrier
results stay with the provider. Swapped callbacks, a replaced result and an
early effect must fail the observer. Native cases reject changed full slots,
bounds, declarations, visibility, method scope, arity and exception contracts.

The separate seed factory oracle also supports `--bounded-carrier`. Its two
original Object-formal scopes are accompanied by a third formal with a fixed
nominal bound, used in a complete invariant input beside the result formal.
Source and frozen-provider profiles retain six classes/eleven methods and five
classes/nine methods. Fifty-six fresh JVM scenarios with two attempts compare
original and recompiled behavior, including null and identical bounded inputs,
both seeds, parent reads, constructor publication and partial final fields,
arbitrary returned carriers and original exception order. Full Signatures and
all other metadata remain unchanged. An independently altered input write must
fail alongside the original swapped-seed, replaced-result and early-field
counterexamples. Native cases separately reject omitted or changed parent,
seed, result, carrier, nominal bound, scope, ABI and exception relations.

`test_mobile_android_collection_predicate.py` compiles an invariant list input
and a contravariant predicate with a covariant closed Boolean result. Source
and frozen-provider profiles retain four classes/seven methods and three
classes/five methods with no metadata projections. Each compares 64 fresh JVM
scenarios with two attempts before and after recompilation through a source
client, immutable typed caller, reflection and method handles. An immutable
iterator observer checks exact hasNext/next/predicate/remove order, both boolean
results, remaining element identities, input mutations, nulls and empty lists,
partial removals, read-only and failing iterators, and exception identity. Three
wrong programs must fail. Native cases cover all eight primitive result carriers
and reject missing invariant anchors, unavailable declarations and altered
method/class scopes or complete callback bounds.

`test_mobile_android_flat_pair_transfer.py` compiles a covariant binary input
and invariant Map result with two independent Object-bounded formals. Source
and frozen-provider profiles reconcile four classes/eleven methods and three
classes/nine methods without metadata projections. Each compares 192 fresh JVM
scenarios with two attempts before and after recompilation through source and
immutable callers, reflection and method handles. Both getter mutations, null
inputs and values, partial writes, existing/null/incompatible provider results,
and late key/value casts and exact failures remain original. Swapped getter
effects, a replaced result and an early effect must fail the observer. Native
cases separately verify input variance and inherited facade references, while
rejecting changed slot order, bounds, arity, scope and exception contracts.

`test_mobile_android_nested_conditional_arguments.py` uses two genuine new
sites whose inner constructor arguments perform two conditional selections.
Source and frozen-provider modes reconcile five classes/eleven methods and two
classes/four methods. Each compares 176 fresh JVM scenarios with two attempts
before and after recompilation through direct/frozen calls, reflection and
method handles. Outer then inner class initialization remains before repeated
getters, selected value identity and partial constructor publication remain
original, and nulls, exact failures and poisoned initialization match. Early
getters, hoisted inner allocation and swapped fields must fail the observer.
Native cases cover wide arguments, nested ownership and complete source/dependency
paths, while rejecting receiver use, escaped branches, inaccessible constructors,
changed outer arguments and protected regions.

`test_mobile_android_dual_map_transfer.py` compiles two covariant Map inputs
and an invariant Map result with the same complete independent formals. Source
and frozen-provider modes reconcile three classes/six methods and two
classes/four methods with unchanged metadata. Each compares 64 fresh JVM
scenarios with two attempts through direct/frozen calls, reflection and method
handles before and after recompilation. Both input identities, equal-key
collision order, mutable fresh maps, partial copies and failures, nulls, aliased
inputs and arbitrary existing/null/input returns remain original. Swapped
copies, a replaced existing result and an early copy must fail the observer.
Native cases verify every input variance combination and inherited facade
references, rejecting partial, repeated, reordered or incorrectly bounded
second input slots.

`test_mobile_android_unseeded_nominal_carrier.py` compiles a covariant pair
containing an invariant unary carrier and the same scoped formal, and retains a
fixed public nongeneric result. Source and frozen-provider modes reconcile six
classes/ten methods and five classes/eight methods with no metadata projections.
Each compares 64 fresh JVM scenarios with two attempts before and after
recompilation through direct/frozen calls, reflection and method handles.
Carrier/seed identity, original and overriding reads, partial fields, nulls,
alternate results, late result casts and exact exceptions match; changing the
returned field between attempts remains visible. Lost seeds, a replaced result
and an early read must fail the observer. Native cases verify direct/nested
trees and reject changed declaration bounds, static scopes, result genericity,
visibility, arity and exception contracts.

`test_mobile_android_intersection_callback.py` uses genuine Kotlin output that
upcasts the original self value before reading a closed callback and invoking
a class/interface intersection helper. Source and frozen-provider modes retain
eight classes/fourteen methods and seven classes/eleven methods with full
Signatures and unchanged metadata after independently recorded D8 CLASS
annotation loss. Each compares 44 fresh JVM scenarios with two attempts through
direct/frozen calls, reflection and method handles. Getter and provider class
initialization order, node/callback identity, null callback/results, state
changes and exact exceptions remain original. A constant-pool interface change
preserves all Code and descriptors; the immutable observer confirms failure at
the original later provider cast, and recovery rejects the missing nominal
witness. Native cases reject missing bounds, callback scope/arity and alias
proofs while preserving the original single-argument intersection coverage.

`test_mobile_android_captured_field_names.py` uses genuine Kotlin class-based SAM
captures, then changes one field-name constant to a JVM-legal name containing
Java-illegal hyphens. Every original Code attribute and descriptor remains
byte-identical. Source and frozen-provider modes reconcile four classes/eight
methods and three classes/five methods, including complete annotation-aware
metadata, original Exceptions and all generated helpers. Each compares 40 fresh
JVM scenarios with two attempts through direct/frozen calls, reflection and
method handles. Wide bits, independent closure identity, null captures, callback
state and exact exception order match. Lost or changed wide values and a replaced
object capture must fail the immutable observer. Native checks require the common
original constructor-capture receipt and reject absent synthetic/final storage,
encoded values, overwritten captures and undeclared field identities.

Only a proven owned synthetic final capture receives a legal source field name.
`field_source_bindings` lists the exact original identity, descriptor, access,
source spelling and constructor parameter receipts. Declaration, constructor
writes and every read share that map. Field-name reflection, binary field ABI and
the synthetic flag cannot be reproduced by Java source; those differences and
retained language metadata are disclosed in the binding.

`test_mobile_android_private_anchored_nominal.py` retains a private generic
instance dispatcher in an owned nongeneric class. An invariant unary input fixes
its method formal; a variant callback and all fixed parameters keep their full
original types. The fixed nominal interface result is independent of that
formal. Source and frozen-carrier modes reconcile seven classes/eleven methods
and six classes/nine methods with unchanged full metadata, original Exceptions
and helper accounting. Each compares 56 fresh JVM scenarios with two attempts
through direct/frozen calls, reflection and method handles. Receiver/input
identity, overriding getters, callback state, arbitrary existing/null results,
boolean/float/wide parameters and exact failure order match. Replacing the result,
losing the receiver and skipping the getter must fail the immutable observer.
Native cases reject foreign or inherited-alias callers, generic declaring
classes, a missing invariant anchor, public instance slots, changed bounds or
result arity, varargs, exception changes and declaration-only private targets.

`test_mobile_android_star_comparable.py` retains two values of one method formal
bounded by `Comparable<?>`, with an unchanged primitive comparison result.
Source and frozen-provider modes reconcile five classes/twelve methods and three
classes/seven methods, including the original regenerated Comparable bridge,
complete metadata, Exceptions and helper inventory. Each compares 44 fresh JVM
scenarios with two attempts through direct/frozen calls, reflection and method
handles. Identity shortcuts, null ordering, receiver and argument identity,
original late bridge casts, exact failures and mutable comparison values match.
Invoking the inherited static method initializes its package-private declaring
class and leaves the public facade uninitialized, including poisoned initializer
behavior. Removing the identity shortcut, reversing null ordering or swapping
comparison receivers must fail the immutable observer. Native source/dependency
and facade cases reject additional bounds/formals, invariant or mismatched input
relations, owner formals, private slots, varargs and changed Comparable
contracts. Only this two-value comparison proof admits the unbounded argument;
existing recursive-bound and source-projection consumers retain their original
super-T requirement.

`test_mobile_android_stateless_covariant_names.py` uses genuine Kotlin value-class
parameters and a stateless anonymous initializer with a covariant result. The
interface, narrow implementation and original broad compiler bridge share one
source name through the existing complete virtual-family proof. A common scope
receipt proves the anonymous member has no captured state or enclosing formals
and one original empty Object constructor; nominal covariance and the exact
forwarding body remain owned by generic inheritance analysis. Source and
frozen-trace modes reconcile eight classes/twenty-four methods and seven
classes/twenty-two methods with complete annotation-aware metadata, Exceptions
and helper accounting. Each compares 24 fresh JVM scenarios with two attempts
through direct/frozen calls, reflection and method handles. Wide bits, singleton
receiver identity, arbitrary existing/null results and exact failure order match.
Lost result, wide value or receiver must fail the immutable observer. Class and
method names and the omitted class-only EnclosingMethod attribute are disclosed;
javac retains the regenerated bridge/synthetic flags. Original primary synthetic
constructors retain their descriptor/body while their flag loss is recorded,
including constructors outside a this-delegation chain. Native cases reject
captured fields, changed constructors/enclosures, borrowed formals, extra method
scope and altered forwarding receiver, target or return. Existing recursive
comparison and ordinary virtual-family coverage remains in the full suite.

`test_mobile_android_dual_seed_bounded_factory.py` compiles a public static
factory with two method formals, two direct seed values, a bounded invariant
converter, an invariant seed input, a contravariant callback with a fixed nominal
tail, and an invariant seed result. Source and frozen-provider profiles retain
eight classes/thirteen methods and seven classes/eleven methods. Each compares
76 fresh JVM scenarios with two attempts across original and recompiled stages
through source, immutable typed, reflection and method-handle callers. Exact
seed/converter/callback identities, null inputs and results, arbitrary provider
results, partial effects, late typed bridge casts and exception order match.
All original metadata, Signatures, Exceptions and helper inventories are checked.
Four wrong programs must fail the independent observer. Native cases reject
incomplete seeds, changed full slots, variance, bounds, callback tails, source
scopes and method access or exception contracts. The existing three-formal
split-parent factory rule remains separately covered.

The fixed nominal mask-constructor runner also supports `--package-subclass`.
A real K2 subclass and raw synthetic constructor retain their original Code,
descriptors and metadata; a witnessed two-byte class flag edit removes only
public visibility. Immutable callers use the original package access, including
reflection and method handles. Source and frozen-default profiles reconcile eight
classes/ten methods and six classes/six methods, including a plain diamond
interface graph on the parent. Each compares 128 fresh JVM
scenarios with two attempts at both stages, observing parent construction,
lazy defaults, partial fields, callback identity and exact failures. Every
original metadata entry and helper is checked, and four independent compiler
counterexamples must fail. The only source projection remains the disclosed
synthetic constructor flag loss. Native negatives reject unknown or generic
parent or interface scopes, cyclic or invalid nominal ancestry, malformed ABI and missing final or
same-owner delegation evidence.

`test_mobile_android_allocation_long_bitwise.py` compiles two nested allocations
with original long AND, OR, XOR, left, signed-right and unsigned-right shifts.
Long operands and int shift distances come from ordered observable providers.
Source and frozen-provider profiles reconcile five classes/ten methods and four
classes/six methods. Each compares 72 fresh JVM scenarios with two attempts at
both stages, retaining high bits, negative and large shift distances, provider
and constructor order, partial final fields and exact exception identities.
All metadata, Exceptions and helper inventories remain unchanged. Three wrong
programs must fail: upper-bit truncation, signed instead of unsigned shift and
early final-field publication. Native cases cover both opcode forms and reject
broken wide words, incorrect operand types, unsupported literal forms, throwing
arithmetic and uninitialized receiver observations.

The package mask runner's `--register-mask` profile uses a genuine K2 Boolean
default with required remaining parameters. Independent D8 disassembly must
contain `and-int/2addr`, followed by the original same-owner constructor call.
The class itself also retains a plain interface alongside its known parent
interface graph. Source and frozen-provider profiles preserve eight classes/ten
methods and six classes/six methods. Each compares 128 fresh JVM scenarios with
two attempts at both stages; Boolean mask choice, parent construction, original
callback identity, partial final fields and failures match. Metadata and helper
inventories are reconciled as in the other mask profiles. Four compiler-produced
counterexamples fail: changed Boolean default, unexpected consumer allocation,
eager unrelated provider initialization and delayed final field publication.
Native cases retain the shared final-initialization receipt for both register
AND forms and reject malformed arity, wide/scoped operands, self observation
and unsupported throwing preparation.

The package mask runner's `--closed-parent` profile retains an explicit Kotlin
parent Signature whose bounded formal is fully fixed by the nongeneric child.
Source and frozen-provider profiles preserve eight classes/ten methods and six
classes/six methods. Each compares 128 fresh JVM scenarios with two attempts at
both stages, including exact parent effects, default callback identity, lazy
initialization, partial final fields and original failures. Full class and
constructor Signatures and other metadata remain checked. The same four
independent compiler counterexamples fail. Native cases reject open owner
formals, wildcard edges, unknown or inaccessible arguments, parent scope and
missing original class Signature; unbound generic parent ancestry stays rejected.

The fixed nominal mask runner's `--star-bounded` profiles use a real Kotlin
star input whose generic declaration retains a known nominal class bound.
Source and frozen-default modes preserve five classes/nine methods and three
classes/five methods, including the original bounded declaration, wildcard
constructor and raw synthetic mask entry. Each compares 128 fresh JVM scenarios
with two attempts at both stages. Default identities, lazy initialization,
partial final fields, original callbacks and exceptions remain matched; every
original metadata and helper entry is checked. Four compiler counterexamples
must fail. Native cases reject unknown or inaccessible bounds, generic bound
leaves, missing declaration scopes and non-star bounded substitutions. A known
interface in a JVM class-bound position remains invalid.

`test_mobile_android_bounded_owner_label.py` independently compiles a class with
one Object formal and one known nominal class-bound formal. An original raw
three-argument constructor forwards two seeds and an invariant bounded binary
converter plus a fixed String to its fully typed constructor. A witnessed
access-only patch adds the raw constructor's synthetic flag; Code, Signatures
and descriptors stay byte-identical. Source and frozen-converter profiles retain
five classes/eight methods and four classes/seven methods. Each compares 32
fresh JVM scenarios with two attempts at both stages through source, immutable
callers, reflection and method handles. Incoming identities, nulls, constant
label, partial final fields and exact failures match. All original entries,
metadata and Exceptions are reconciled; only the reported synthetic constructor
flag changes. Three wrong programs must fail. Native cases reject changed seed
or carrier relationships, missing bounds or scope, receiver/argument copies,
non-synthetic entries, unsupported preparation and checked exceptions.

The bounded owner label runner's `--mask` profiles retain the same exact class
bounds in a raw six-argument default constructor. Independent Java Code and
metadata are preserved by synthetic-flag-only witnesses for both original raw
constructors; the marker is a separately authored fixture declaration. Source
and frozen-converter profiles retain six classes/ten methods and five classes/
nine methods. Each compares 64 fresh JVM scenarios with two attempts at both
stages, including both threshold/label default selections, null labels, incoming
identities, partial final fields and exact failures. Only the two disclosed
synthetic flags change. Four wrong programs must fail, including a lost default
mask. Native cases reject divergent carrier bounds, variance, wide/receiver
origins, non-synthetic entries, malformed marker ABI and exception contracts.

`test_mobile_android_paired_seed_bounded_result.py` retains both method formals
in an invariant binary factory result, with an invariant unary input, bounded
binary converter and three direct Object-formal seeds. Source and frozen-factory
profiles preserve seven classes/fourteen methods and six classes/twelve methods.
Each compares 56 fresh JVM scenarios with two attempts at both stages. Existing
and null provider results, input reads, all seed and converter identities,
constructor publication, partial final fields and original failures match.
Full class/method metadata, Signatures, Exceptions and helper inventories are
unchanged. Four wrong programs must fail. Native cases reject altered result
slots, carrier bounds/variance, insufficient seeds, visibility and exception
contracts. The unary result and three-formal split-parent profiles keep their
independent proof and tests.

An anonymous captured class may retain its superclass constructor proof when
reported shared class or method projections affect only other declaration slots. The
original superclass edge and each selected incoming argument subtree must be
unprojected, and the exact parent declaration, parameter erasure and original
incoming register identity remain required. The existing self-consumer proof
still requires `Sink<? super Self>` against `Sink<Object>` with the declared
Object bound; it does not admit changed consumer types or generic parent edges.

`test_mobile_android_projected_capture_super.py` independently verifies bounded
token and wide captures before parent initialization, parent virtual dispatch,
callbacks, partial final fields and exact failure identity. Source and dependency
profiles retain seven classes/fifteen methods and six classes/twelve methods,
respectively. Each compares 45 cases twice through immutable callers, reflection,
method handles and source calls, and rejects three independently wrong programs.
Every original declaration and helper is reconciled after the reported shared
member binding and four upper-bound projections. Inputs use Java 25; only the
external parent declaration's class-file major version changes to Java 17 for
loading, with identical Code, descriptors and all remaining attributes. The
frozen parent logic remains unchanged. Coverage stays `partial`.

The `--method-scope` variants use a static generic factory whose formal shadows
the host's class formal. Its exact method scope remains in all four projection
receipts. `--object-bound` also checks Object erasure, retaining seven classes/14
methods or six classes/11 methods with the parent as a dependency. Both bounds
keep the same 45-case behavior matrix, complete metadata reconciliation and
three incorrect programs. Class and method captures share one admissibility
rule; neither may change the selected parent argument subtree.

An exact public static nullary method with a raw reference result may also
supply a closed generic superclass argument. Its immediate result must enter a
fresh local and retain the selected descriptor. The shared carrier-tree proof
checks all known declarations, Object bounds and complete closed wildcard trees;
method or class formal inference remains unsupported on this path. The original
getter call stays in the Java 25 prologue, including its effects and failures.
The `--static-result` profiles add a raw getter and a contravariant parent slot,
checking 50 cases twice, getter failure before parent entry, parent callbacks,
partial captures, and five incorrect programs including skipped or repeated
getter calls. All original declarations and metadata remain reconciled.

A declared static void consumer can bind one Object formal and one fixed nominal
bound through two complete invariant input carriers. Both original input slots
must have the same formal order and match their independently declared canonical
bounds. Unknown declarations, wildcard or closed substitutions, extra formals,
varargs and checked exceptions fail. The common bounded-carrier proof retains
original erased arguments and dispatch without inferring provider behavior.
`test_mobile_android_bounded_pair_consumer.py` preserves six classes/eighteen
methods, or five classes/sixteen methods with a frozen provider. Each profile
checks 64 cases twice through source, immutable callers, reflection and method
handles, including virtual input reads, partial state, wide values, nulls and
exact failures. Four independently wrong programs fail the frozen observer.
All original metadata, Exceptions, descriptors and reported helper inventories
remain unchanged.

The same complete Object/nominal input-pair receipt can accompany a wide argument,
a nested pair consumer callback and a closed completion consumer when the result
is the original plain Object descriptor. Both callback declarations retain their
Object bounds; the nested invariant pair matches the input order, and callback
result and completion input share one known nongeneric nominal type. Covariant
consumers, changed nested bounds, mismatched completion types and unknown
declarations fail. This proof does not interpret suspension or replace a result.
`test_mobile_android_bounded_pair_callback.py` independently retains ten classes
and 23 methods, or nine classes and 21 methods with the provider in a frozen JAR.
Each profile compares 104 cases twice, including partial fields, wide values,
virtual input reads, callback/completion order, arbitrary or null returns and exact
failures. Four wrong programs fail frozen observers. Original metadata,
Exceptions, descriptors and helper inventories remain reconciled in full.

A static extractor may consume a unary producer containing a callback of one
Object-bounded method formal and a fixed nominal result, then return the same
callback declaration invariantly. The full producer, consumer and result trees
must agree, including their Object-bounded declarations and fixed result type.
Owned private methods retain this same relation; ordinary access checks still
require the declaring scope. Unknown declarations, variance or bound changes,
extra formals and foreign private calls fail.
`test_mobile_android_nested_fixed_callback.py` retains five classes/eight methods,
or two classes/five methods with immutable interface/result declarations. Each
profile compares 50 cases twice through source, frozen callers, reflection of
both accessor and private method, and method handles. It checks getter count,
callback identity and dispatch, nulls, failed casts and exact exceptions, rejects
three wrong programs, and reconciles all original metadata and helper inventory.

The nested extractor receipt also admits an invariant unary carrier inside its
producer and result. It uses the same complete declaration, method formal and
access proof; unary input and output arguments remain invariant. The nested
callback runner's `--unary` profiles preserve four classes/seven methods, or two
classes/five methods with immutable declarations. Each checks 50 cases twice
through the same five frozen call routes, including getter count, returned
identity, callback dispatch, nulls, casts and exact failures. Three independently
wrong programs fail the observer, and all original metadata, Exceptions and
helper inventories remain unchanged.

A same-interface default accessor can retain its existing private instance body
helper when the interface has unused generic formals. The original default and
accessor must both lack method Signatures, and their receiver, parameter/result
words and dispatch target still match exactly. Interface source projections or
changed method type contracts fail. The original body stays in its semantic
scope, preserving internal virtual calls while compatibility dispatch bypasses
an override. The interface-default runner's `--generic-owner` profile uses
unchanged Kotlin compiler output with an Object formal and a known nominal bound.
It retains eight classes/fourteen methods and compares 80 cases twice through
legacy and frozen callers, reflection and method handles. Default and virtual
dispatch, captures, local enclosure, partial effects and exact exceptions match;
three wrong programs fail the frozen observer. Full metadata is reconciled apart
from the existing reported helper, local binding and accessor flag changes and
eight independently checked D8 CLASS annotation losses. Coverage stays `partial`.

An invariant converter can bind an Object input and one known nominally bounded
return formal through the same canonical bounded-carrier slot proof. The direct
seed must use the Object formal, both converter declaration bounds must match,
and the returned formal must retain its original nominal descriptor. Owned
private static calls also require the ordinary declaring-scope access check;
external private calls and incomplete or variant slots fail.
`test_mobile_android_bounded_converter.py` independently retains five classes
and eight methods, or two classes/five methods with immutable converter,
callback and bound declarations. Each profile compares 65 cases twice through
source, frozen callers, public/private reflection and method handles. It checks
null shortcuts, getter/callback count, original arguments, arbitrary bounded
returns, nulls, failed casts and exact exceptions. Four wrong programs fail the
frozen observer; all original metadata, Exceptions and helper inventory remain
unchanged.

The bounded owner constructor receipt can retain one exact independent unary
interface edge using its Object formal invariantly. The interface declaration
must be public, Object-bounded, unprojected and without additional interface
parents; the full original edge, formal identity and erasure remain checked.
The constructor still selects its exact same-owner target, while inherited
members keep their independent complete contracts. Unknown, variant, widened or
bounded-formal interface edges fail. The bounded owner runner's
`--mask --interface-owner` profiles retain seven classes/twelve methods, or six
classes/eleven methods with the converter declaration frozen. Each checks
64 cases twice, including default values, partial final fields, interface result
identity and complete interface reflection. Five wrong programs fail the frozen
observer. Original Code and metadata remain unchanged except the two separately
witnessed and reported synthetic constructor flags; coverage stays `partial`.

A seeded bounded factory needs no extra unary seed carrier when its invariant
binary result already retains the converter input's complete Object/nominal
formal pair. Two direct Object seeds remain required, and both independent
carrier declarations must match the same scoped formal order and canonical
bounds. Unary results and separate-parent profiles retain their earlier anchors.
`test_mobile_android_converter_seed_pair_factory.py` independently retains six
classes/eleven methods, or five classes/nine methods with a frozen provider. Each
profile compares 56 cases twice through source, frozen callers, reflection and
method handles. It checks two wide words, Boolean values, converter/seed identity,
partial fields, arbitrary/null returns and exact failures. Four wrong programs
fail frozen observers; all original metadata, Exceptions and helper inventories
remain unchanged.

The same exact binary input/result relation can retain one direct Object seed and
one direct nominally bounded seed. Both must use their own original method
formals and descriptors; a plain value with the same nominal erasure is not a
seed proof. The invariant carrier pair still anchors both result slots, and unary
results keep their earlier restrictions. The converter-seed runner's
`--bounded-input` profiles retain six classes/eleven methods, or five classes/nine
methods with a frozen provider. Each compares 56 cases twice, including both
seed identities, two wide words, Boolean values, partial fields, original/null
returns and exact failures. Four wrong programs fail frozen observers. Complete
original metadata, Exceptions, descriptors and helper inventories remain intact.

Final-field planning composes closed single-store preparation stages with a
structured multi-store suffix. Switches in the preparation still cannot skip or
revisit a required store, and every suffix path writes each remaining final once.
Shared else guards may use an inverted conditional followed immediately by the
original jump to the common arm; the shared plan records that direction and the
emitter preserves short-circuit evaluation and intervening effects. Cross-cut or
cross-arm entries, repeated/missing writes and unknown jump destinations fail.
A common capture already emitted in a verified Java 25 constructor prologue is
not emitted again when the structured suffix starts after the parent call.
The shared-else runner's `--switch-prefix` profiles retain three classes/eight
methods, or one class/one method with unchanged effect/value dependencies. Each
compares 96 cases twice, including wide final values, partial field snapshots,
short-circuit reads and exact failures. Five wrong programs fail frozen observers.
All original metadata, Exceptions, runtime entries and helpers are reconciled;
compiler input Code and frozen callers remain unchanged. Native cases additionally
check inverted guards and the pre-parent capture path.

A wholly unwritten instance final can retain its typed allocation default in a
public or package final class with plain Object preparation. Its source scope
must be independent or have an existing verified anonymous-to-named-member binding.
Every primary constructor must begin with its exact Object constructor call;
verified same-owner delegations reach those primary constructors. The shared
field-write inventory resolves original symbolic owners and rejects any known or
unresolved matching write. An effect-free nonconstant source initializer runs
after Object preparation and before every original body operation. It introduces
no helper, callback or ConstantValue and is disclosed in
`default_instance_final_initializers`. Primitive zero/false and reference null
share the static-field default literal authority, including positive floating
zero. Unknown preparation, user superclass behavior and nonfinal owners retain
explicit initialization proof.
`test_mobile_android_unwritten_instance_boolean.py` uses a witnessed final-field
flag edit with identical original Code, descriptors and all other attributes.
Source and frozen-observer profiles retain four classes/eight methods and three
classes/five methods. Each compares 36 cases twice across mutable compiler input,
encoded original and recompiled stages, checking allocation defaults, loops,
partial wide values, field/getter reads, reflective mutation during construction
and exact failures. Four wrong programs fail frozen observers. All metadata and
ConstantValue absence remain matched apart from the witnessed original final flag.

`test_mobile_android_unwritten_typed_finals.py` adds all eight primitive types,
Object and array defaults in a genuine anonymous member. Only class, field and
InnerClasses final flags are encoded; all original Code and other bytes remain
unchanged. Owned and SDK-interface profiles retain three classes/24 methods and
two classes/14 methods. Each compares 32 fresh JVM cases twice at all three
stages, including raw NaN/negative-zero bits, reference identity and repeated
reflective mutation through direct, frozen, reflection and method-handle calls.
Five incorrect programs fail frozen observers. All original definitions and
metadata are reconciled with the explicit anonymous source binding.

An explicitly typed constructor can call one declared interface conversion before
its exact same-owner primary constructor. The input receiver's Object formal and
the conversion method's known nominal bound are substituted through the same
full contract specializer, matching the original converter input and vector
result trees. The remaining three incoming references and the receiver retain
exact origins. Unknown declarations, result/formal drift, changed operands,
varargs and checked exceptions fail. Same-arity constructor separation requires
known unrelated first-argument nominal roots. The common bounded owner receipt
also retains a complete invariant binary interface edge with both independent
canonical bounds; unary edges keep their earlier Object rule.
`test_mobile_android_typed_vectorizing_constructor.py` preserves eight classes
and fourteen methods, or three classes/eight methods with immutable interface,
converter, vector and bound declarations. Each compares 44 cases twice through
source, frozen callers, reflection and method handles, observing one conversion,
original input/result identity, partial fields and exact failure order. Three
wrong programs fail frozen observers. All metadata, Exceptions, descriptors and
helpers remain unchanged; Java 25 retains the original pre-delegation evaluation.

The typed conversion constructor proof also retains a declared converter getter,
its invariant callback input/result pair, and the original nominal result cast.
The callback class independently proves two Object bounds. Both getter and
callback contracts are specialized through the original receiver/class scopes;
the callback consumes the original fourth input and returns the primary
constructor's bounded formal. The original check-cast must establish that exact
nominal descriptor before delegation. Extra calls, wrong operands, changed result
formals, missing/widened casts and checked exceptions fail.
The typed vectorizing runner's `--callback-input` profiles retain nine classes and
sixteen methods, or three classes/eight methods with immutable contracts. Each
compares 68 cases twice, including getter/callback count, original input and result
identity, nulls, failed casts before primary construction, partial fields and exact
exceptions. Four wrong programs fail frozen observers. Metadata, Exceptions and
helper inventories remain intact; original pre-delegation evaluation uses Java 25.

Protected final instance methods on independent nongeneric source classes may
receive a spelling-only identifier projection when their complete ancestry and
unchanged signature are known. Abstract declaring classes are supported. Each
call proves its original lookup, including inherited symbolic references, and
checks access against the actual declaring package. Cross-package calls retain
the exact lexical self receiver; same-package calls retain the original typed
receiver. Missing ancestry, competing original-name declarations and unproved
constructor-prefix access fail clearly. The original method identity and the
fresh source spelling are reported together; access flags, full generic
signatures, argument evaluation and returned values stay unchanged.

An owned anonymous local within an independently renamed closed method uses
the same enclosing-method mapping as a complete virtual family. Its original
and source enclosing identities are both reported. The enclosing metadata of
external declarations and named local classes must retain its original method
name and continues to block this spelling projection.

A complete protected virtual family may share a Java identifier when rooted at
an owned protected abstract class method. All participating declarations keep
one plain descriptor, full unchanged scopes, known ancestry and original
visibility. This spelling receipt preserves the complete override family;
cross-package self calls validate the lexical family member and keep virtual
dispatch. Independent final entries retain their separate proof. Missing or
external family declarations, competing overloads, scoped signature changes
and unproved caller access fail clearly.

Plain public abstract class methods can also root complete naming families.
An unrelated interface bearing the same original JVM name keeps its own
resolved declaration receipt; abstract class and interface entries may share
one fresh source spelling without merging their runtime contracts. Generic
class or method scopes require their existing specialized proof. A super call
to an abstract declaration remains unsupported and fails before source export.

A public static void method may retain raw carrier inputs of one known nominal
class-bound formal. The shared recursive input proof checks each nested type,
variance, declaring scope and independent carrier bound against the original
erased descriptor. Plain auxiliary declarations and primitive parameters retain
their exact types. The complete Signature remains unchanged; invocation adds
no callback adapter, result inference or stronger cast. Unknown bounds, scoped
carriers, synthetic/varargs entries and checked throws remain unsupported.

Pure constructor preparation that copies its uninitialized receiver selects
the existing immutable allocation graph, including a different range-call
receiver or a chain of object copies. The graph retains one allocation origin,
original parameter words and every initialized alias before later register
reuse. Its existing checks reject uninitialized consumers, changed constructor
origins, external entries and incompatible exception state.

An exact two-operation compiler marker constructor on a final class with a
known closed generic superclass can forward to its private fixed constructor.
The complete parent edge, nominal bounds and callback parameter trees remain
unchanged. A nonpublic type argument must be an owned top-level declaration in
that same source package; dependency-only and nested private types gain no
access. The marker keeps its original raw descriptor and reported synthetic
flag difference, with the original receiver and argument words preserved.

The scoped-mask constructor runner's `--allocated-default` profiles use
unmodified Kotlin and D8 output where a conditional default allocation replaces
an incoming reference parameter. Owned and dependency defaults retain lazy
class initialization, sticky initialization failure, repeated allocation and
constructor failures, callback identity and partial final fields. The frozen
observers compare 128 fresh cases twice and reject five wrong programs. DEX
register witnesses independently identify the overwritten reference slot.
Every original definition, descriptor and metadata field is reconciled after the
existing disclosed synthetic constructor flag; all recovered Java compiles.

The typed-config constructor runner's `--formal-input direct` and
`--formal-input array` profiles forward the unchanged owner formal to the primary
constructor before the original field updates. Each owned/dependency profile
compares 56 fresh cases twice, checking nulls, input identity, float raw bits,
partial final fields, ordered effects and exception identity through frozen
callers, reflection and method handles. Four incorrect programs fail the
observers. Original signatures and all other metadata remain unchanged; every
recovered Java source compiles. Native checks also cover nominal bounds, copies,
range calls, input order and distinct formals sharing an erasure.

The seeded-closed-input runner's `--nested-result` profiles remove direct formal
seeds and retain a `Pair<Box<T>,T>` result, closed input carriers and a consuming
callback. Static profiles compare 112 cases twice; `--instance-provider` adds
null receivers for 116 cases twice. Owned and dependency providers keep original
class initialization, callback order, object identity, late checked casts,
mutable inputs, partial state and exact failures. Seven wrong programs fail
frozen observers. All original entries and metadata match and every recovered
Java source compiles. Native checks additionally cover nullary/unary factories,
inherited aliases, range calls, source order and incomplete nested relations.

Public generic calls can also bind a one-dimensional array result through one
invariant unary input carrier, such as `Class<T>` in `T[] read(int, int, Class<T>)`.
The method has one Object-bounded formal, with the same scoped variable in the
carrier and array component; other arguments must be primitive. Complete carrier
arity, bounds and source accessibility remain mandatory. The original provider,
receiver, arguments and returned array are retained, including later caller casts
and their failure order. Static, virtual and interface calls share this proof;
raw/star/variant inputs and incomplete component relations remain unsupported.

A selected nested constructor inside a conditional allocation can retain pure
register preparation before its initializer, including reference and wide copies
into a contiguous range-call frame. The shared linear/conditional classifier
admits only local copies, numeric constants, NOPs and existing total integer
preparations. Ordinary verification still checks every typed operand, wide pair,
uninitialized receiver and constructor access. Effectful preparations, handlers,
external entries and cycles remain outside this bounded proof. The outer
allocation and selected inner allocation keep their original initialization order.

A final owner with a complete closed class scope may retain a raw package-visible
synthetic mask constructor delegating to its original private constructor. Public
mask entries may select the same private target too. The common delegation proof
still owns every branch and uninitialized-this copy, and the signature validator
checks complete nested parameter contracts, exact erasures and overloads. The raw
entry gains no generic Signature or visibility change; the private target keeps
its original body and Signature. Only Java's unrepresentable synthetic flag is
reported as a metadata difference.

Reference array stores preserve the actual array's runtime component check. An
Object value may be stored through an Object[] view of a proven reference array,
including an array of primitive subarrays. Both ordinary statements and ordered
constructor argument graphs use the same source view; no component cast is
inserted before the store. Original null, index and component failures, array
identity and partial writes remain observable. Array reads and primitive stores
retain their exact element typing, and malformed operands still fail validation.

Closed marker-constructor arguments may contain known nested lower, upper and
unbounded wildcards on abstract and open owners as well as final owners. The
synthetic entry still forwards every original input word in two instructions to
a private same-owner target. The shared recursive contract proof checks every
nested declaration, arity, bound and accessible type; it introduces no generic
Signature on the raw entry. The private target's body, complete Signature and
subclass callback behavior remain unchanged.

Guarded same-owner constructor prefixes can retain exact scalar conversions
before their selected provider calls. The constructor planner consumes the same
source/destination type table as ordinary instructions and allocation graphs;
Body still verifies operand types and emits the original Java conversion.
Both source and destination widths participate in uninitialized-receiver checks,
including implicit high words inside a nested allocation prefix. Unknown
conversions, malformed frames and receiver overlaps fail clearly. The selected
conversion, provider initialization and final-field writes retain their order;
these guarded source prologues require Java 25.

A public static carrier transfer may bind one Object-bounded method formal
through a unary input, both consuming slots of a binary callback and the same
invariant unary result carrier. The callback's result is a complete known
nongeneric nominal type. The signature planner proves every declaration, bound,
variance, scoped occurrence and erasure before reusing the shared recursive
input proof. The source keeps the actual provider call and its opaque returned
reference; callback invocation, scalar unboxing, later caller casts and class
initialization remain in their original bodies. Raw/star/incomplete carriers,
foreign formals and unmatched producer/result relations do not gain a binding.

The same carrier-preserving proof can retain a unary key selector with a second
Object-bounded method formal. The input and invariant result still use the same
scoped input formal; the selector consumes that formal and produces the distinct
key formal. Full declaration arities, bounds, variances and original erasures
are checked before this receipt bypasses the separate producer-to-mapped-result
relation. The provider retains the key's opaque Object erasure and every later
explicit cast or unboxing operation, including its failure order.

A complete conditional child allocation may remain in an outer constructor's
argument program before later reference or scalar arguments. The conditional
planner checks every original child instruction against the outer receiver,
wide words and complete forward CFG, retaining bounds, initializer access and
external-entry/handler rejection. The outer region owns its complete source
range while the child proof remains available to the common argument emitter.
Both original allocation/class-initialization sites retain their order; the
child executes at its own initializer before subsequent outer argument effects.
Target metadata on non-branch preparation still rejects. These argument programs
use Java 14 switch expressions and retain the ordinary typed operand verifier.

A public static two-input generic call with a fixed nonvoid result may bind its
sole Object-bounded method formal through a complete known covariant unary
producer. Nested unary layers retain invariant arguments; every input carrier,
formal scope, declaration arity, accessibility and bound still passes the shared
signature proof. The original invocation and raw argument erasures remain in
the body. This adds no return conversion or prediction of callback behavior.
The collection predicate fixture's `--covariant-values` profile independently
authors and compiles the original wildcard input and verifies both source and
SDK provider calls, all Java, entries, metadata, Exceptions and runtime effects.

Conditional constructor argument programs also admit the exact `cmp-long`,
`cmpl-float`, `cmpg-float`, `cmpl-double` and `cmpg-double` instructions through
the same classifier as ordinary Body verification. The destination stays an
int, both wide input words must avoid the uninitialized receiver, and invented
opcode suffixes reject. Comparison expressions retain their original NaN and
signed-zero behavior. The nested conditional tail fixture's `--comparison-tail`
profiles compile independently authored original Java and observe both long
and floating comparisons after an opaque call, before later volatile argument
reads and final-field stores. Source and SDK variants cover NaNs, infinities,
subnormals, signed zero, extrema, aliases and original exceptions. All emitted
Java, original entries, metadata and Exceptions must still match.

Owned private static and instance methods may reuse the complete nested
callback/completion Object-formal binding proof. Each callback input, nested
completion, output and final completion retains its declaration, variance,
arity and bound; the fixed Object result can still be a suspension sentinel.
This receipt changes no access rules: Body checks the exact original private
receiver, invocation style and declaring class at every call. External private
members and interfaces gain no binding. The scoped completion runner's
`--private-provider` variants independently compile private Java declarations
and same-owner public wrappers, witness the retained D8 private access and call,
and check both source and SDK contract declarations. Deferred capture, actual
results, null receivers, partial effects, exceptions and all original metadata
are compared against frozen original callers.

Conditional allocation ownership follows a bounded set of reachable forward
control-flow nodes, rather than every instruction physically between `new` and
its initializer. Every normal path must reach one exact initializer; no external
jump, fallthrough or handler may enter an owned argument node. Disjoint sibling
branches remain independently verified and emitted. Nested regions retain their
actual node sets when composing a parent, so interleaved alternatives are never
removed by a physical range skip. Structured final-assignment emission still
requires contiguous ownership. Lookahead identifies initializer shape without
resolving a member before ordinary verification owns its diagnostic.
The interleaved allocation fixture compares independently authored Java and JVM
bytecode before D8, witnesses the retained disjoint blocks, and checks every
output Java and original metadata against frozen callers. Source and SDK
constructor variants cover both sibling allocations and a parent around a
noncontiguous child, original initialization order, partial final stores,
opaque long results, null paths, exceptions and sticky initialization failures.

A conditional parent may compose a fully proved linear child argument program,
including its original field reads and calls. Ordinary linear roots still use
their existing value graph. An unreachable trailing alignment `nop` provides no
physical entry into an argument region; reachable fallthrough beyond the code
boundary remains an error in ordinary verification.

A conditional allocation may finish with one exact reference move forwarding
its uninitialized receiver into a constructor frame. Every initializer path must enter that
copy before the initializer; its following bounded adapter contains only pure
preparations, and neither receiver word may be consumed or overwritten. Both
implicit wide words retain the same checks. The ordinary initializer verifier
still requires the original allocation PC in the copied register and initializes
all surviving aliases. Constructor and allocation handler coverage remain empty
for this proof. The pure allocation receiver-copy runner's conditional profiles
compare independently authored smali with a frozen Java specification, preserving
conditional wide values, both boolean slots, original identity and partial final
stores; smali input is explicitly not described as compiler output.

Bounded constructor argument regions also admit packed and sparse switches with
terminal throw arms. Normal paths retain one exact accessible initializer;
throw-only paths may replace the unused allocation register after proving that
its original uninitialized identity is never read or copied. The complete
argument program stays inside the original `new` expression at Java release 14.
`test_mobile_android_switch_allocation.py` checks source and SDK declaration
profiles, allocation and class initialization before selection, captured argument
identities, default exception construction, failed initialization caching and
partial constructor stores. Every recovered Java unit is compiled, original
definitions and metadata are compared, and independent wrong programs test the
observer.


`test_mobile_android_reference_array_super.py` compiles owned Java 25 constructor
prefixes that allocate and fill reference arrays before a generic superclass
call. Source and SDK declaration profiles each compare 92 fresh JVM cases with
two attempts, compile every emitted Java unit and retain complete original
metadata and Exceptions. Frozen observers witness distinct ordered elements,
array component identity, zero and negative lengths, null receivers, partial
final stores, exact errors and failed class initialization. Eight signature-only
mutants and ten independent wrong programs challenge the contract and observer.


`test_mobile_android_covariant_array_super.py` retains a nested
`Map.Entry<? extends K, ? extends V>` component in the same original reference
array construction loop and superclass call. Source and SDK profiles each
compare 92 fresh JVM cases with two attempts, all generated Java25 units,
original definitions and full metadata/Exceptions. Eight signature-only mutants
reject mismatched scope, variance, raw components and unused formals. Ten
independent wrong programs challenge ordered allocations, reference identities,
array effects, partial stores and exact failures using unchanged observers.


`test_mobile_android_allocation_int_shifts.py` compiles all output for source
and SDK declaration profiles with register or post-initialization field inputs.
Each profile compares 64 fresh JVM cases with two attempts and complete original
metadata/Exceptions. A frozen bit oracle uses repeated unsigned-long arithmetic
to check signed-int left, arithmetic-right and logical-right shifts with masked
negative and large distances. Nine independently compiled wrong programs
challenge distance width, sign, input snapshots, literal values and ordering.


`test_mobile_android_inherited_generic_allocation.py` retains descriptor-only
protected final array/scalar calls through a complete unchanged generic class
chain in the same runtime package. Source and SDK profiles each compile every
Java8 output and compare80 fresh JVM cases with two attempts, full metadata and
Exceptions. Frozen observers witness two original array snapshots, receiver and
index mutation, allocation/class initialization order, partial final fields and
exact errors. Six signature-only mutants and eight independent wrong programs
challenge access admission, contract completeness and runtime observations.


`test_mobile_android_paired_plain_array.py` retains a private static method's
independent Object-bounded seeds beside a plain Object array and scalar context,
returning the original plain array descriptor. Source and SDK profiles each
compile every Java8 output and compare72 fresh JVM cases with two attempts,
original metadata and Exceptions. Frozen observers witness input/result aliases,
independent/null seed values, ordered partial array writes, target initialization
and exact throws. Six signature-only mutants and eight independent wrong
programs challenge the binding and observer.


`test_mobile_android_paired_array_indices.py` retains independent scalar
auxiliaries beside both generic seeds and the closed array context/result.
Source and SDK profiles each compile all Java8 output and compare72 fresh JVM
cases with two attempts, full original metadata and Exceptions. The frozen
observer witnesses both index roles, partial writes, arrays and throwable
identity. Six signature-only mutants and eight wrong programs include reusing
the first index for the second store. Native cases cover zero, two, three and
eight scalar auxiliaries, all scalar word types, declaration scopes and frames.


`test_mobile_android_concrete_super_resolution.py` retains a lexical superclass
call through two nearer classes with abstract interface slots and a farther
concrete class declaration. Source and SDK profiles each compile all Java8
output and compare56 fresh JVM cases with two attempts, complete original
metadata/Exceptions and helpers. Frozen observers witness receiver, argument
and boolean-result snapshots, initialization, partial commits and exact throws.
Eight independent wrong programs challenge virtual recursion, receiver changes,
field-write order and delayed argument/result reads. Native controls retain the
nearest abstract override and reject malformed ancestry, descriptors and frames.

`test_mobile_android_private_interface_result.py` checks an owned private static
helper with one Object element formal and one result formal whose sole bound is
a known nongeneric interface. A complete unary producer and binary callback bind
the element, while exactly one direct argument binds the result formal. Fixed
nominal and primitive auxiliaries retain their original descriptors. The proof
requires original unprojected declarations, complete Object bounds on both
carriers, an owned body, and the normal exact private caller and owner checks.
It does not infer how the helper uses its inputs or which value it returns.

Source and SDK-input profiles independently check original DEX definitions,
compile all emitted Java, compare metadata and Exceptions, and run 88 fresh JVM
cases twice through frozen callers. They cover arbitrary, null and input result
identity, callback and append effects, partial state, initialization failure,
nulls and delayed casts. Five Signature-only controls and eight independent
wrong programs must fail. The interface-only bound is not rewritten as a class
bound, and the shared class-bound validator retains its existing policy.

`test_mobile_android_producer_callback_nominal.py` checks the same complete unary
producer and binary callback binding for a public static helper returning a
fixed known nongeneric nominal type. Its sole Object formal has no direct seed;
the producer and callback must use exactly that scoped formal. The input role
proof is shared with private interface results. The public admission separately
requires an independent nongeneric owner, original unprojected signatures,
complete declarations, a real owned body or SDK declaration, and no unsupported
flags or throws. Fixed auxiliaries retain their original erasures.

Both source and SDK-input profiles compile all generated Java and compare all
original definitions, metadata and Exceptions. Each uses 88 fresh JVM cases
twice, five same-code Signature controls and eight wrong implementations,
including an early result cast and a substituted input result. No provider
behavior or returned identity follows from the generic binding proof.

`test_mobile_android_allocation_long_arithmetic.py` checks total long addition,
subtraction and multiplication inside nested constructor arguments. Ordinary
statements, linear allocation graphs and conditional allocation admission share
the exact long binary opcode descriptor with the existing bitwise and shift
operations. Both operands of arithmetic remain long, including signed int-to-long
conversions, and results retain wrapping 64-bit semantics. Division/remainder
are outside this total allocation family.

Source and SDK-provider profiles independently account for original definitions,
compile all generated Java and compare complete metadata and Exceptions. Each
runs 72 fresh JVM cases twice through frozen callers, covering extrema,
overflow, signed conversion, all six result fields, provider order, partial
final fields, repeated allocation identity and failures at every effect
boundary. Eight wrong implementations must be detected, including narrowing,
changed operators, repeated calls, lost sign extension and replaced exceptions.

`test_mobile_android_private_callback_element.py` checks an owned private static
helper whose sole Object formal is both the method result and the output of a
complete binary callback. The callback accepts a fixed known nongeneric nominal
input invariantly or contravariantly and returns the own scoped formal
invariantly or covariantly. Its independent nongeneric declaring owner, live
body, original flags/signatures/throws and normal exact private caller scope
remain required. The existing recursive input proof checks complete declaration
bounds and accounts for the same formal; no callback behavior or returned
identity is inferred.

Source and SDK-input profiles compare all definitions, metadata and Exceptions,
compile every emitted Java file and run 72 fresh JVM cases twice. They observe
context snapshots and mutation, arbitrary or null results independent of the
callback result, sticky initialization failure, original exception identity and
the caller's delayed String cast. Five same-code Signature controls and eight
independent wrong implementations must fail with frozen observers.

`test_mobile_android_context_callback_element.py` extends the private callback
result corpus with independent closed auxiliary arguments. Exactly one complete
callback supplies the own result formal; each remaining parameter must be a
known accessible nongeneric nominal type or a primitive, at its original
erasure. The same private declaration, callback variance, recursive bound and
caller scope proofs apply when no auxiliary is present.

Source and SDK-input profiles freeze all original inputs and observers, compare
all definitions and metadata/Exceptions, and compile all emitted Java. Each runs
84 fresh JVM cases twice, including full-width auxiliary snapshots, mutation,
null field-read failure order and actual returned identity. Five Signature
controls and ten wrong programs cover the original callback cases plus copied
auxiliary identity and narrowed snapshots.

`test_mobile_android_bounded_callback_element.py` checks an owned private static
helper whose callback output and method result share one known nongeneric class
bound. The class-bound validator retains its existing policy. Its callback and
closed auxiliary input proof is shared with the Object-bound profile; recursive
formal checks use the original nominal erasure. Original private caller scope,
live body, flags, throws and unprojected declaration requirements remain.

Both source and SDK-input profiles compile all generated Java and compare all
original definitions, metadata and Exceptions. Each runs 80 fresh JVM cases
twice, five Signature-only controls and eight wrong programs. A wrong Object
reply fails at the helper's original bound cast after callback effects; an
instance of the bound class fails later at the caller's subtype cast after its
result snapshot. Callback values and actual method replies remain independent,
including null and input aliases. No additional cast is inferred from binding.

`test_mobile_android_raw_factory_delegation.py` checks a no-argument constructor
of a public final generic class that obtains one raw unary carrier from a static
factory and delegates to its same-class generic constructor. The original class
has one Object-bound formal and retains its complete interface/header scope.
The selected parameter and every one-argument constructor sibling must have
complete unprojected fixed-arity contracts. A selected unary carrier binds that
same class formal invariantly or covariantly through a known Object-bound
declaration. Method-formal overloads and varargs are excluded.

The bounded prefix proof tracks one static factory result and original
uninitialized-this aliases through exact reference copies; it permits no this
argument, branch or unknown preparation. Body separately validates the factory
call, generic invocation, exceptions and effects. A cast to the complete
selected constructor parameter retains original overload selection without
predicting the returned object or any element type.

Source and SDK-input profiles compare every definition and metadata/Exceptions,
compile all generated Java, and run 60 fresh JVM cases twice through frozen
callers. Four Signature-only controls and eight wrong programs cover carrier
identity, mutation, partial final fields, repeated allocation, nulls, factory
initialization and original exception identity.

`test_mobile_android_nested_lock_scopes.py` checks genuinely nested monitor
acquisitions and separate inner/outer finally callbacks. A complete CFG stack
proof must preserve the exact acquisition stack at every normal and exceptional
join, pop its innermost acquisition at each release, and retain the existing
must-alias proof for the acquired object. Nested synchronized dispatchers return
to their immediate enclosing dispatcher after release. Single-scope profiles
retain their existing proofs.

The source and SDK-callback profiles compare all original definitions and
metadata/Exceptions, compile all emitted Java, and run 80 fresh JVM cases twice.
Frozen callers inspect `Thread.holdsLock` at every callback for distinct and
reentrant objects, null acquisitions, partial state and exceptions replaced by
finally blocks. Eight wrong programs exercise omitted, swapped or extended
lock scopes, duplicated callbacks, lost state/finally effects and changed
exception identity.

`test_mobile_android_private_consumer_result.py` checks a private instance
consumer returning its owner's own private static nongeneric member class.
The independent nongeneric owner and original unprojected member declaration
must be owned; a dependency-only, captured, generic or foreign member gains no
access. Its one complete Object-bound binary callback accepts the same own
method formal invariantly or contravariantly and yields a fixed known nominal
result invariantly or covariantly. The shared recursive input proof retains
complete bounds and exact erasures, and Body retains exact private caller,
declaring owner, receiver and dispatch checks. Global nominal accessibility
rules are unchanged.

Source and SDK-input profiles compare all original definitions and full
metadata/Exceptions, compile every emitted Java file and run 80 fresh JVM
cases twice. Frozen observers distinguish caller and peer receiver identity,
receiver mutation and snapshots, callback and private-member casts at their
original positions, arbitrary/null results and exceptions. Four Signature-only
controls and eight wrong implementations must fail. The binding predicts no
callback execution or returned-object identity.

`test_mobile_android_final_diamond_aborts.py` checks blank final assignments
through conditional arms with explicit exception exits before or after the
common continuation. The shared initialization plan carries complete incoming
write counts into both arms. Only normally completing arms must agree; a path
ending in its original throw may leave later fields unassigned, but cannot
write an already assigned final again. Conditional exits to physically trailing
exception blocks require an exact linear throw-only entry without final stores.
The source emitter consumes those same ranges and retains every original
read, call, assignment and throw in its branch.

Owned source and SDK-provider profiles compile every emitted Java file and
compare128 fresh JVM cases twice through frozen external callers. Observers
check partial initialization, getter/volatile snapshots, null/reference aliases,
long bits and runtime/error identity. Fourteen independently compiled wrong
programs must fail; all original definitions, metadata and Exceptions stay exact.

`test_mobile_android_allocation_prefix_copies.py` checks original register
copies between a constructor-prefix allocation and its exact initializer.
Narrow, reference and wide copies share one exact shape/frame receipt across
captured forwarding and linear emission. Source types and complete wide words
must remain available; copies may expose neither uninitialized receiver.
The original accessible constructor and its unchanged arguments still execute
once, retaining class initialization, allocation identity and failures.

Owned Java8 source/SDK profiles retain ten typed overloads with independent
input parameters and genuine D8 copies. Every emitted Java unit compiles, all
original definitions and metadata/Exceptions stay exact, and64 fresh JVM cases
run twice across four caller routes. Frozen observers check both argument
origins, long/float/double bits, null/array identity, distinct wrapper objects
and partial runtime/error failures. Fourteen wrong programs must fail.

`test_mobile_android_floating_constructor_prefix.py` checks total floating
arithmetic before this-delegation, including chained sums and normalization.
A single exact opcode family and whole-word origin proof covers ordinary,
allocation and constructor contexts. Each source result retains its original
float/double rounding point and ordering; division and remainder at zero retain
floating NaN/infinity behavior without changing integer exceptions.

Owned source/SDK profiles compile all generated Java25 units, compare every
original definition and complete metadata/Exceptions, and run64 fresh JVM cases
twice through four external caller routes. Frozen original bytecode supplies
numeric output, with arithmetic NaN payloads normalized and finite/infinite/
signed-zero result bits exact. Getter snapshots, partial final fields and exact
runtime/error identities remain observable. Fourteen wrong programs must fail.

`test_mobile_android_synthetic_interface_roots.py` uses genuine Kotlin output targeting JVM8, with
`@JvmSynthetic` abstract/default contracts and two concrete implementations with
non-Java method names. A complete owned family may share one disclosed Java
spelling. The original interface root, all runtime descriptors and access,
known ancestry and source scopes remain authoritative; a synthetic flag alone
cannot rename an SDK member or authorize a body or bridge.

Source and SDK-callback profiles compile all emitted Java units and compare96
fresh JVM cases twice through frozen reflective, MethodHandle and genuine Kotlin
callers. Full metadata/Exceptions accounting permits only the four reported
source names and synthetic flag losses plus independently checked D8 CLASS
annotation loss. Wide argument words, raw float bits, null receivers, default and
inherited lookup, object identity and partial callback failures are observed.
An unchanged SDK root remains rejected; fourteen wrong callbacks must fail.

`test_mobile_android_nominal_super_constructor.py` checks a closed nominal input
forwarded into a generic superclass constructor whose class formal erases to
Object. The original superclass Signature binds the argument; the child retains
its descriptor and absent constructor Signature. Admission requires a known
public nominal declaration, an Object-bound parent formal, the exact incoming
reference and a unique fixed-arity target. Competing overloads, open or unknown
types and unproved register origins remain rejected.

Both source and SDK-parent profiles compile all emitted Java8 units and retain
all original definitions, complete declaration metadata and Exceptions. Frozen
external callers check 144 fresh JVM cases twice, including null/subclass
identity, long boundaries, escaped partially initialized fields and exact
runtime/error failures. Sixteen independent wrong programs must fail first.

`test_mobile_android_guarded_closed_delegation.py` checks guarded this-delegation
into complete closed constructor types. The shared constructor planner owns all
prefix paths and uninitialized-this aliases; the generic planner retains the
selected full parameter types and every fixed-arity competitor. Open formals,
source projections, variable arity, checked target throws and malformed physical
words keep their existing rejection paths. A synthetic flag supplies no type
information, and a descriptor-only caller gains no Signature metadata.

Source and SDK-parent/interface profiles compile all emitted Java25 units and
retain original definitions, supported declaration metadata and Exceptions.
Frozen callers run 180 fresh JVM cases twice across all masks and four routes,
observing null/shared/unchecked lists, raw scalar bits, partial construction and
exact runtime/error failures. Sixteen independent wrong programs must fail first.

`test_mobile_android_scoped_suspend_forwarder.py` checks unmodified Kotlin JVM8
suspend interface defaults and their generic static bodies. The canonical paired
callback/completion proof validates each of up to two preceding closed nominal
or primitive arguments; an explicit receiver plus a long/double preserves the
original descriptor and full physical words. No method-name heuristic or new
Signature projection is used. Source and SDK-context profiles compile all emitted
Java9 (the reported private-interface-helper minimum), compare every original definition and supported declaration metadata, and
run 168 fresh JVM cases twice with frozen external callers. Sixteen wrong programs
check the independent identity, raw-bit, dispatch and exception-timing assertions.
Ordinary synthetic method bodies that otherwise retain their complete source
contract receive a `synthetic-method` receipt when Java cannot reproduce that
access flag. The receipt changes the recovery status to partial and preserves
the original descriptor, Signature, Exceptions and executable body.

`test_mobile_android_closed_nominal_overload.py` checks genuine Kotlin mask
constructors selecting a descriptor-only nominal parameter beside a closed
generic callback overload. A complete nongeneric Object-only nominal header
proves the selected static view cannot convert to the unrelated generic carrier.
The existing constructor-overload separation rule retains explicit descriptor
casts even for null and objects implementing both interfaces; no constructor
Signature is invented. Source and SDK-handler/context profiles compile all Java
at the reported release, compare all original definitions, metadata and Exceptions,
and run 240 fresh JVM cases twice. Eighteen wrong programs validate independent
mask, identity, snapshot, partial-field and exception-timing assertions first.

`test_mobile_android_fixed_completion_factory.py` checks static unary and seeded
callback factories returning one closed completion carrier. The existing complete
callback/completion graph validates independent Object-bound method formals,
carrier declarations, variance and exact erasures; the returned carrier has one
fixed nominal argument. Source and SDK-factory/carrier profiles compile every
Java8 unit and compare all definitions, supported metadata and Exceptions without
Signature adjustment. Frozen external callers run 168 fresh JVM cases twice,
including null/aliased/arbitrary results, unary/binary dispatch, optional execution,
partial effects and exact failure timing. Eighteen wrong programs must fail first.

Canonical empty `package-info` inputs produce `package-info.java` files containing
only their named package declarations. Recovery keeps each original class identity
in `package_metadata_classes` and reports
`java_compiler_options: ["-Xpkginfo:always"]`. Pass these options to `javac` when
rebuilding the source tree; otherwise an empty package declaration may produce no
classfile. This retains the original synthetic abstract interface flags without
inventing a source type or annotation. Metadata with members or annotations,
noncanonical flags and ordinary type references remains unsupported explicitly.
See the [javac package-info generation option](https://docs.oracle.com/en/java/javase/26/docs/specs/man/javac.html#extra-options).
