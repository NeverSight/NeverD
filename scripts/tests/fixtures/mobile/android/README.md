
`plain_interface_covariance` keeps class and interface Signatures absent while
preserving two original covariant return bridges. Source and frozen interface
profiles each compare 180 calls, including 80 expected failures, through typed,
root-interface, intermediate-interface, reflection and method-handle callers.
An immutable subclass inherits the original bridges and overrides only their
typed targets to witness virtual dispatch; mutable arrays, exact wide
argument bits, null results and original failure identities expose forwarding
changes. SDK disassembly witnesses both original virtual bridges. Every original
runtime entry, access flag, Signature and Exceptions attribute is checked;
additional source helpers are reported. Optional fields in both interfaces
exercise inherited/shadowed ownership with reflection and method handles. Source
constants retain their values through the existing reported field initializers,
with precisely checked ConstantValue absence; frozen SDK constants stay intact.
A same-Code method-formal mutation must fail recovery, and an independently
compiled wrong-result implementation must fail the immutable observer.
The abstract-interface profile adds an original bridge to an abstract typed
member. A frozen implementation inherits that bridge and exercises it through
root, typed, reflected and method-handle calls: 280 calls and 140 expected failures
per profile. Its SDK profile freezes only the plain root so the source interface's
original bridge remains verifiable. Native cases vary class visitation order,
metadata presence, range encoding and every forwarded register origin.
The re-abstracted-default profile gives that root a concrete default instead.
Its counter must stay untouched through every call; removing the original
interface bridge makes the immutable observer detect the wrong parent fallback
and must also fail recovery. Both owned and frozen default roots are checked.

`private_result_invocations` keeps an original private result-only generic
method in a captured member class. Its fixed arguments include `Class<?>[]`,
object arrays, longs and doubles. Source and frozen provider profiles each
compare 288 calls, including 248 expected failures, through direct, immutable,
reflected and method-handle callers. An immutable subclass defines a namesake
to witness private dispatch; a distinct receiver, mutable arrays, nulls, NaN
payloads, signed zero and original result casts expose changed arguments or
failure order. Runtime inventories, Signatures and Exceptions stay unchanged;
the existing capture ownership/field-flag projection is precisely disclosed.
Same-Code Signature counterexamples change formals and reifiability, while
independently compiled receiver replacement and omitted result checks must
fail the original observer.

`protected_allocation_preparations` uses ordinary javac/D8 string arguments
between allocation and construction in one protected region. Source and frozen
provider profiles each compare 40 fresh JVM cases with two attempts, including
typed and catch-all handlers, explicit throws, constructor failures, repeated
class-initialization failures and handler failures. Immutable callers,
reflection and method handles observe literal messages, original exception
identities and unchanged mutable or null handler inputs. Complete runtime
inventories and metadata survive unchanged. Independently changed literals,
handler identities and allocations moved outside protection must fail the
observer; native tests retain rejection of split handler ranges and deferred
register reads.

`recursive_void_carrier` retains a javac/D8 recursive Comparable bound on a raw
list void consumer. Source and frozen sorter profiles each compare 120 fresh
JVM cases with two attempts through ordinary, frozen, reflected and method
handle callers. The observers check every comparison pair, list identity,
sorted mutations, null/noncomparable elements, unmodifiable lists, exact
failures, first/repeated initialization and monitor ownership. Original class
and method inventories, Signatures and Exceptions remain unchanged. Five
independent compiled counterexamples expose bypassed/repeated calls, copied
carriers, missing synchronization and changed caught identities.

`seeded_nominal_results` checks a static generic factory whose original seed
and callback carrier accompany primitive varargs and a fixed nominal result.
Source and frozen provider/facade profiles each compare 60 fresh JVM cases with
two attempts, using an immutable callback with a real compiler bridge. The
observers check result and token identity, array identity, negative zero and NaN
payloads, declaring-class initialization, late bridge casts and callback failures.
Complete generic signatures, varargs flags, metadata and Exceptions must remain
unchanged. Three same-Code metadata mutations remove the seed, add a method
formal or give the result its own formal; six compiled controls alter callback
count, array identity, result identity, cast timing, failure identity or zero bits.

`seeded_scalar_carrier` retains an unchanged javac/D8 generic search with
`List<? extends Comparable<? super T>>`, an original `T` seed and an integer
result. Source and frozen provider profiles each compare 120 fresh JVM cases
with two attempts. Frozen direct callers, reflection and method handles check
an independent list's precise size/read order, input identity, mutable comparison
counts, result snapshots, null/wrong inputs and exception identity. Plain,
protected and synchronized routes retain first/repeated initialization failures
and monitor ownership. Complete inventories, nested method Signatures and
Exceptions remain unchanged. Seven independently compiled counterexamples
expose skipped/repeated searches, changed seeds, copied lists, late results,
missing monitors and altered caught failures.

`package_generic_defaults` retains a package static `<T> T` helper with an
original formal default argument and raw result. Its 132 fresh JVM cases each
run twice through ordinary, frozen, reflected and method-handle callers. Plain,
protected and synchronized routes check default reads before provider class
initialization, exact result snapshots, finally-state observations, later caller
casts and mutable input replacement. Null/wrong values, caught and uncaught
failure identities, first/repeated initialization and monitor releases remain
exact. Complete inventories, Signatures and Exceptions are unchanged. Six
independently compiled controls expose omitted/late defaults, changed results,
missing finally effects, changed catches and an early result cast. A same-Code
private access mutation must be rejected without changing the immutable caller.

`unused_formal_scalar_names` retains a real Kotlin static integer method with
one unused method formal and a Java provider with a namesake owner formal. Both
source and frozen provider profiles check 40 fresh JVM cases with two attempts.
Class initialization mutates the input array after its scalar argument is read;
immutable callers, reflection and method handles expose changed snapshots,
integer limits, call order and exact initialization/callback failures. Original
Signatures and flags survive the disclosed name projection. Same-Code Signature
counterexamples and independent bypass/repeated-call bodies must be rejected.

`allocation_scalar_conversions` checks scalar field reads and numeric conversions
inside ordinary, protected and synchronized constructors. Both source and
frozen provider profiles compare 120 fresh JVM cases with two attempts each.
Initialization changes incoming fields; a later callback overwrites them after
their values have been captured. NaNs, infinities, saturation, signed zero,
narrow integer truncation, nulls and exact failures expose changed evaluation.
Seven independently compiled counterexamples must fail the frozen observers.
Original inventories, metadata and Exceptions remain unchanged.

`allocation_scalar_results` retains long, double, float, byte, short, char and
boolean calls between allocation and construction. Ordinary, catch-protected
and synchronized entries are tested in source and frozen provider profiles;
each profile compares 144 fresh JVM cases with two attempts. Later callbacks
overwrite prior scalar fields, while immutable argument snapshots, NaN payloads,
signed zero, narrow integer bits and surrogate chars remain exact. Class
initialization precedes every argument effect; original constructor, callback,
null and initialization failures retain their identity and release the lock.
Inventories, metadata and Exceptions stay unchanged. Seven independently
compiled counterexamples expose omitted calls, long narrowing, floating zero
changes, inverted boolean results, missing monitor ownership and changed catch
identities. Native tests also check copied and paired words across static,
virtual, interface and superclass dispatch.

`protected_allocation_cleanup` retains a javac/D8 allocation whose catch body
is itself covered by finally. Source and frozen provider profiles each compare
80 fresh JVM cases with two attempts through direct, immutable, reflected and
method-handle callers. Conditional cleanup, constructor and first/repeated
initialization failures, catch callbacks and cleanup failures preserve their
order and exact exception identity. Original runtime inventories, class and
method metadata and Exceptions remain unchanged. Four independently compiled
counterexamples remove a finally effect, change the caught identity, move
allocation outside protection or invert the cleanup branch. Native tests also
reject deferred register observations, cyclic cleanup and graphs beyond the
256-instruction bound.

`captured_allocation_arguments` allocates a named member with linear and
conditional argument effects. Source and frozen provider profiles each compare
112 fresh JVM cases with two attempts through direct, immutable, reflected and
method-handle callers. A superclass callback observes the original outer before
child initialization; mutable identities, null reads, initialization poisoning
and every call/constructor failure retain their order. SDK disassembly witnesses
the capture before super and the original argument programs. Runtime inventories,
Signatures and Exceptions stay unchanged; only the existing disclosed static
ownership and synthetic capture-field flags differ. Independently changed
captures and hoisted reads must fail the immutable observer.

`conditional_nested_allocations` composes an outer construction, adjacent inner
initialization, fluent calls and an original conditional argument. Source and
frozen provider profiles each compare 160 fresh JVM cases with two attempts:
both branches, nullary and incoming-reference inner constructors, direct and
immutable callers, reflection and method handles, every initialization/call/
constructor failure, null string values and mutable identities. The oracle
also checks platform exception messages formed with a conditional class name.
SDK disassembly independently witnesses both allocation sites and the branch;
complete inventories, Signatures, fields and Exceptions must survive unchanged.
Independent inner-allocation and branch hoists must fail the ordering oracle.

`raw_receiver_invocations` uses unchanged javac/D8 generic classes and interfaces
whose instance methods share class and method formals across input and covariant
result carriers. Both source and frozen provider profiles compare complete
runtime inventories, Signatures, fields and Exceptions. Immutable callers,
reflection, method handles and external overrides witness 224 calls plus 64
platform `Class.asSubclass` calls, including 192 expected failures. The observer
checks actual receivers, mutable identities, long high words, NaN payloads,
negative zero, nulls and effect/exception order. A same-Code class-bound drift
must fail recovery. Independent changed-receiver, truncated-word and skipped
platform-check variants must fail the behavior oracle.

`erased_result_invocations` uses ordinary javac and D8 originals to exercise
single-bound result-only method formals with unchanged mixed reference and
wide parameters. Both the source-provider and frozen SDK-provider profiles
recompile all recovered sources and compare complete class/method inventories,
Signatures, access and Exceptions. The independent harness covers class and
interface bounds, static and virtual dispatch, external overrides, discarded
calls, original narrow result casts, nulls, raw NaN bits, mutable aliases and
provider-before-cast failures through direct, reflected, method-handle, frozen
and recovered callers. Extra-formal and intersection-bound metadata-only
counterexamples preserve Code and descriptors but must fail before publication.
An independent javac counterexample checks incompatible inferred result bounds.

`nominal_result_invocations` extends the ordinary compiler-derived result-only
profile to invariant `Carrier<T>` returns. Source and frozen provider/carrier
profiles each compare 148 calls and 71 expected failures, including virtual
overrides, raw container identity, unused results, later element reads and casts,
mixed wide inputs and original failure order. Complete metadata and runtime
inventories stay unchanged. Three same-Code generic-constraint mutations must
fail recovery, an incompatible inferred result must fail javac, and a separately
compiled eager element read must fail the frozen observer.

`captured_covariant_bridges` uses ordinary javac/D8 public and private inner
classes that inherit an interface through a plain abstract superclass. Source
and frozen ancestry profiles each compare 168 calls and 105 expected failures.
The frozen subclass omits only its compiler bridge to exercise the original
inherited bridge and actual virtual override. Captured owner identity, changing
arrays, indices, result identity and exception order are checked through direct,
interface, reflected and method-handle routes. Runtime inventory and metadata
are retained except the existing disclosed static-member and capture-field
source differences. A same-Code method-formal mutation is rejected, and a
separately compiled changed result must fail the immutable observer.

`anonymous_covariant_bridges` keeps an ordinary javac/D8 anonymous subclass's
captured owner, forwarded superclass argument and covariant return bridge.
Source and frozen superclass/interface profiles each run 48 fresh JVM cases
through recovered, immutable, reflected and method-handle callers. They check
owner and token identity, constructor and callback effects, and first/repeated
initialization failures. Complete inventories, metadata and Exceptions are
compared with only the reported local name and enclosing-instance projection
differences. A same-Code method-formal mutation must fail recovery; four compiled
controls independently change the owner, forwarded token, constructor callback
or failure identity and must fail the frozen observer.

`anonymous_member_allocations` keeps a field-free anonymous subclass with
class-only enclosure, a specialized generic superclass and both original getter
and setter bridges. Four profiles cover linear or conditional constructor
arguments with source or frozen superclass/name providers. Each compares 72
fresh JVM cases with two attempts, checking allocation and class initialization
order, argument snapshots before callback mutation, bridge casts, setter effects
and first/repeated failures through ordinary, frozen, reflected and method-handle
callers. Complete inventories, generic contracts, metadata and Exceptions retain
only the disclosed named-member binary identity and enclosure differences.
The descriptor-only anonymous constructor uses a proven cast at its call to the
closed generic parent while retaining its original Signature metadata.
A same-Code superclass specialization mutation must fail recovery; five compiled
behavioral controls must fail the immutable observer.

`captured_constructor_reads` retains named inner constructors that store their
outer capture before reading mutable `int` and `long` superclass arguments.
The release D8 witness requires an `sget` to reuse the incoming outer register;
native counterparts also cover overlapping wide writes. Source and frozen
provider profiles each run 56 fresh JVM cases with two construction attempts.
They verify scalar snapshots before later class initialization, captured owner
identity during the superclass constructor and on failed objects, and exact
callback and exception order. Complete inventories, metadata and Exceptions
retain only the reported static-member and capture-field differences. Six
compiled controls include a capture saved after the superclass callback and
a static field read delayed until after its original value changes.

`implicit_static_defaults` uses twelve originally unwritten mutable static
fields, then changes only their classfile final flags. The original owner has
no initializer and all method Code, descriptors and attributes remain unchanged.
Source and frozen parent/trace profiles each run 25 fresh JVM cases with two
attempts, verifying typed defaults, parent initialization, reentrant reads,
inherited field aliases, and first/repeated failures. Rebuilt inventory allows
only the explicitly reported owner initializer in addition to existing helpers;
ConstantValue absence and newly compiled consumer bytes remain unchanged.
An independent constant-inlining control loses parent initialization; four
other compiled controls change floating zero, a callback, a marker or failure
identity and must fail the immutable observer.

`instance_intersection_invocations` keeps inherited virtual and interface calls
with one two-interface method formal. Source and frozen provider profiles each
compare 320 calls through ordinary, immutable, reflected and method-handle
callers, including a frozen overriding subclass. They verify the original
argument field snapshot before a mutating callback, typed null arguments, null
receivers, dispatch and exact exception order. Signatures, metadata, Exceptions
and runtime inventories remain unchanged. A same-Code missing-bound mutation
retains the callee's late cast but must fail recovery; an independently added
narrower overload retains original caller bytes and must also fail recovery.
Five separately compiled behavioral controls must fail the frozen observer.

`inherited_covariant_bridge_slots` keeps ordinary javac/D8 bridges on both a
concrete superclass and a subclass with the same typed return descriptor.
Source and frozen ancestry profiles each compare 160 calls and 80 expected
failures through typed, superclass, interface, reflected and method-handle
routes. The immutable subclass inherits the original bridges while overriding
only their typed target. Complete metadata, ordered Exceptions, runtime slots,
wide bits, aliases and effect order are checked after recompilation. A same-Code
extra method formal is rejected; a separately compiled changed result must fail
the immutable observer.

`allocation_static_calls` keeps ordinary javac/D8 allocation, static argument
calls, wide values and optional result casts. Source and frozen SDK-provider
profiles compare complete inventories, Signatures, fields and Exceptions, then
run 32 fresh JVM cases through recovered, immutable, reflected and method-handle
callers. Each profile checks 24 successful constructions and 40 expected failures,
including first and repeated target/provider initialization failures, provider
exceptions, cast failures and constructor exceptions. Every trace distinguishes
allocation initialization, provider initialization, the call and construction;
identities, raw NaN bits and post-construction effects are also checked. A
metadata-only private-provider counterexample must fail recovery, and an
independently compiled hoisted call must fail the runtime ordering oracle.

The allocation-call `--generic-provider` profile compiles an independently owned
`<T> T check(T, long, double)` provider before D8. Both source and SDK variants
retain its exact Signature and rerun the entire initialization, cast, identity
and exception-order matrix. This checks composition of the shared bound/formal
invocation proof with deferred constructor arguments; no runtime encoding edit
or unchecked provider substitution supplies the original behavior.

`nested_allocations` uses ordinary javac/D8 originals with an outer allocation,
inner allocation, virtual getter, inner constructor, static array provider and
outer constructor. A second path builds an exception with an original fluent
StringBuilder chain. Source and frozen SDK-provider profiles compare complete
metadata and inventories, then run 46 fresh JVM cases with immutable callers:
12 successful nested constructions, 28 ordered failures, eight successful input
cast constructions, 12 cast/initialization failures, eight fresh exception
constructions and two original toString failures. Integer-returning SDK getters
supply 72 constructor results, including integer extremes, null filenames and
Unicode labels, with six null-receiver failures. Independent D8 witnesses check
that both getters precede a late string occupying an earlier constructor
argument. The oracle checks first and repeated initialization failures,
initialization before failed casts, external getter overrides, null receivers,
mutable identities and register reuse. A private-provider metadata counterexample
fails recovery, and an independently compiled allocation hoist must fail the
ordering oracle. Fourteen additional JVM cases cover a single allocation
with a virtual getter argument, including class initialization before a null
receiver or failed getter. A constant followed by Object.toString retains its
original invocation before exception construction. A separate linear hoist
counterexample must also fail the oracle.

`interface_initializers` retains a generic interface's complete original fields
and class initializer. Ordinary javac/D8 originals call String.replace and a
provider before reading another class's static field. Fields deliberately sort
differently from their initialization order. Source and frozen SDK-provider
profiles compare complete inventories, metadata and Exceptions, then run 20
fresh JVM cases: 24 successful reads, 16 first initialization failures and 80
erroneous-class accesses. Immutable recovered/frozen callers, reflection and
MethodHandle getters observe initialization cycles, partially assigned fields,
original failure identity and first/repeated access. A same-Code private provider
is rejected; an independently compiled field-order change must fail the oracle.

`erased_callback_invocations` uses an ordinary static member class with private
generic reader and writer methods. The reader accepts Creator<T> and returns T;
the writer accepts a sole interface-bound T and retains its void descriptor.
Original and rebuilt classes execute through direct and frozen callers,
reflection and MethodHandles: 40 successful reads, eight successful writes,
20 ordered read/cast failures and 12 write failures. Checks retain null behavior,
callback identities and state, integer extremes, private reflection and exact
Signatures/Exceptions. Source and frozen SDK callback/input declarations share
that matrix. Same-Code extra-formal and wildcard changes fail recovery, while
an independent javac counterexample rejects an incompatible writer argument.

`monitor_bypass_tails` keeps an unmodified javac/D8 release-mode method with
pre-acquisition branches into late early returns and a shared unlocked callback.
Its immutable caller and observer run 252 fresh JVM cases, with two attempts
each, through typed, frozen, reflection and method-handle calls. Source and
frozen dependency profiles check bypassed and null locks, integer overflow,
ordered callbacks, original failures, first/repeated initialization failures,
and replacement of the lock field while the original monitor remains held.
Complete inventories, metadata and Exceptions are compared after recompilation;
five independently compiled changes to acquisition, bypasses, early returns and
callback placement must be detected.

`monitor_regions` uses an ordinary generic Java class whose explicit synchronized
block lazily creates a payload, invokes callbacks and updates mutable state.
Independent D8 witnesses retain monitor-enter, both monitor-exit operations and
the catch-all cleanup. Source and SDK-payload profiles compare all original
metadata, Exceptions and runtime entries. Frozen callers check 6,000 concurrent
updates, four reentrant updates, two null-lock failures, four ordered failures
and six successful acquisitions by another thread after release. Callback and
body failures retain their original identity. Independently compiling the same
body without synchronized must fail the ownership oracle.

`monitor_sparse_cleanup` reuses that immutable observer with an unmodified
Kotlin/D8 original. SDK catch-table intervals independently prove that a numeric
constant after acquisition and the paired normal release have no catch-all.
Source and frozen payload profiles retain the exact DEX runtime metadata,
including Kotlin Metadata, and run the same concurrency, reentrancy, exception
identity and cross-thread release matrix. Compiler-retained CLASS annotations
are parsed in full and their D8 removal is independently witnessed. Numeric
constants remain real original operations; no positive runtime bytecode is
patched to construct the sparse interval layout.

`allocation_statements` uses an original Java switch expression inside constructor
arguments. D8 linearizes it to allocation, a void static call, class literal and
constructor. Source and frozen SDK-provider profiles retain all metadata and
runtime entries, then run 20 fresh JVM cases with direct, frozen, reflected and
method-handle callers. Eight successful constructions and 32 ordered failures
check target/provider initialization, original exception identity and repeated
erroneous-class access. The class literal must not initialize its target. A
same-Code private provider is rejected; an independently compiled statement
hoist must fail the ordering oracle. Recovery reports Java 14 only for the
statement-bearing expression, without adding helper methods or allocations.

`nested_capture_allocations` compiles two ordinary anonymous generic subclasses
inside an outer constructor's arguments. Source and frozen SDK-base profiles
retain every original runtime entry and compare all metadata against the two
explicit local-class binding receipts. Rebuilt sources use the reported Java 25
release for pre-super capture writes. Twenty-four fresh JVM cases exercise eight
successful constructions and 40 ordered initialization/constructor failures.
Direct, frozen, reflected and method-handle callers check mutable capture aliases,
nulls, original failure identity and fresh object identities. A same-Code private
constructor is rejected, and independently compiling captures before the outer
allocation must fail the initialization-order oracle.

`delegation_argument_calls` uses javac 8/D8 originals whose secondary constructor
computes three float arguments and an integer before calling the primary
constructor that writes every final field. Four virtual getters reload a mutable
static provider, and the first getter replaces it. Source and frozen SDK-provider
profiles retain all entries, metadata and Exceptions, and compile recovered
Java 25 prologues. Thirty-six fresh JVM cases check eight successful objects and
64 ordered initialization/getter/constructor failures through direct, frozen,
reflected and method-handle callers. Raw NaN bits, negative zero, infinities,
subnormal floats and integer extremes are retained. A same-Code private virtual
getter is rejected, and an independently compiled cached-provider variant must
fail the receiver-identity oracle.

The `--private-forwarder` static-name profile adds a genuine Kotlin private
static wide helper called by the same declaring class. Ordinary and synthetic
profiles preserve private access, raw long values, callback order and failures,
while source and frozen allocation targets share the original disclosed name
mapping. A separately compiled foreign Kotlin caller followed by a callee-only
access change must be rejected; naming cannot make a private method inheritable.

`synthetic_method_names` uses the unmodified Kotlin 2.0.21 compiler to produce
three genuine public static synthetic methods with JVM names that Java source
cannot declare. No positive class bytes or annotations are patched. The pinned
compiler archive is fetched from the official JetBrains release in CI; the local
runner accepts its `lib` directory via `--kotlin-lib` and uses JDK 21. All-input
and frozen Box-dependency profiles exercise 60 fresh JVM cases each through
direct, frozen, reflected and method-handle callers. They check nullable and
mutable object identity, long extrema, void calls, allocation/class-init order,
and original exception identity on first and repeated calls. Original runtime
entries map exactly to the disclosed names; the synthetic-bit difference is
explicit. Class, field, Signature, runtime annotation and Exceptions comparisons
remain exact. D8's removal of CLASS-retained annotations is separately witnessed
with SDK dexdump and every removed typed payload is recorded. Removing the
original synthetic flag without changing Code must reject recovery. An
independently compiled wrong void-call effect must fail the immutable oracle.
Native cases also check inherited static references, input order, reserved-name
collisions and unsupported dispatch/contracts.

`final_throw_regions` uses javac 8/D8 originals with a conditional throw tail
physically after the normal return, in both a constructor and a class initializer.
SDK disassembly independently checks the two original writes, branch and tail
layout. All-input and frozen Trace-dependency profiles each run 24 fresh JVM
cases through direct, frozen, reflected and method-handle callers. They retain
original exception identity, throw-null behavior and repeated class-init failure.
An escaped partially constructed object and a circular static-field observation
make the original assignment positions observable. Full original class/method,
field, Signature and Exceptions inventories remain unchanged; generated helpers
are explicitly accounted for, including a reserved helper-name collision.
An independently compiled early-final-write variant must fail the oracle.
Native cases also cover inline throws and reject missing, repeated, skipped or
post-return final assignments and branches into or out of an unproved tail.

`delegation_postludes` uses javac 8/D8 originals with two consecutive this-call
edges, followed by int/long field stores and observable callbacks. SDK
disassembly independently checks both exact leading calls and postlude layouts.
All-input and frozen Trace-dependency profiles each run 48 fresh JVM cases
through direct, frozen, reflected and method-handle callers. Observers inside
the primary constructor and each postlude see original final/mutable field
values, null and mutable reference aliases, int/long extrema, and exact failure
identity. A primary failure must suppress all later postlude effects. Complete
class/method, field, Signature, Exceptions and generated-helper inventories are
checked. An independently compiled callback-before-store variant must fail the
immutable observer. Native direct/linked and reversed-order cases verify the
shared two-edge receipt and reject final rewrites, cycles, missing targets,
invalid postlude operations and broken register/word layouts.

`plain_super_calls` uses unchanged javac 8/D8 originals with a captured child,
a plain intermediate class and a closed generic method on a plain parent.
Source and frozen provider profiles each compare 56 fresh JVM cases with two
calls per case through direct, unchanged binary, reflection and method-handle
routes. Both branches preserve the captured outer, actual receiver, overload,
mutable identities, nulls, integer extremes and failures before and after the
parent's write. Independently compiled wrong virtual dispatch and overload
variants must fail the oracle. Original inventory, class Signatures, method
metadata and Exceptions stay unchanged; the existing captured static ownership
and synthetic field projection is precisely reported.
The `--generic-ancestor` variants repeat both profiles with an actual formal on
the parent and a fixed Object specialization on the intermediate class, while
the captured child still has no class Signature.

`static_super_accessors` uses a genuine Kotlin internal inline superclass call.
The compiler emits a public static synthetic receiver-first accessor that calls
the immediate parent, whose actual implementation is inherited from its parent.
Source and frozen-parent profiles each run 40 fresh JVM cases through direct,
frozen, reflected and method-handle routes. Independent subclasses override the
ordinary virtual slot; every accessor must still reach the superclass body.
Null receivers, first/repeated class-initialization failures, original receiver
and exception identity, full runtime inventories and Kotlin Metadata are checked.
The original static entry remains, with an explicitly recorded synthetic flag
difference and one private instance helper. A same-Code non-synthetic entry is
rejected, and an independently compiled virtual-call variant must fail the
immutable observer.

The `--captured-member` profile uses a genuine Kotlin inner class with an
original explicit outer constructor parameter and synthetic capture field.
Source and frozen-parent modes each run 40 fresh JVM cases with two attempts.
The unchanged caller and independent subclass must retain superclass dispatch,
outer identity, null receiver failures and first/repeated initialization failures.
The existing capture receipt projects the member to static Java ownership and
requires Java 25 for its original pre-super field write. Exact constructor
Signature, member modifiers and capture-field flag differences are checked and
reported alongside the unchanged static accessor and its private helper.
The Kotlin compiler runs separately on JDK 21; rebuilt Java runs on JDK 25 or newer.

The `--object-result` profile retains a nullable superclass return through the
original invoke-super/move-result-object/return-object tail. Source and frozen
parent modes each run 80 fresh JVM cases, with two attempts per case. Frozen
callers, reflection and method handles check exact object or null results,
class-initializer writes, a callback that replaces the result field after its
original read, subclass overrides and exception identity. D8's removal of the
two original CLASS-retained Nullable annotations is recorded independently;
complete Kotlin Metadata and all remaining class and method facts stay exact.
Independently compiled discarded-result and late-field-read variants must fail
the same immutable observer, as must the existing virtual dispatch variant.

`allocation_private_calls` retains real javac 8/D8 private invoke-direct calls
inside ordinary, protected and synchronized construction. Source and frozen
target modes each run 96 fresh JVM cases with two attempts through direct,
frozen, reflected and method-handle callers. A frozen subclass defines public
methods with the same names; private dispatch must continue to the original
declaring-class bodies. The oracle checks object/null identity, full long bits,
wide argument forwarding, initializer writes, ordered input mutations,
constructor publication, exact failures and monitor ownership/release. Complete
original inventories, access flags, metadata and Exceptions remain unchanged.
Six independently compiled changes to calls, results, catch identity or monitor
ownership must fail the same immutable observer.

`protected_final_stages` places single and conditional blank-final stores before
an original nested catch/finally program, with another final store afterward.
Source and frozen provider modes each run 132 fresh JVM cases with two attempts
through direct, frozen, reflected and method-handle constructor calls. The oracle
checks all final values, callback order, caught/replacement failure identities,
cleanup on successful and failing paths, constructor publication and first or
repeated class-initialization failures. Full original inventories, method flags,
metadata and Exceptions remain exact. Five independently compiled changes to
stage order, calls, cleanup or caught identity must fail the immutable observer.

`protected_final_suffix` retains a javac/D8 initializer with typed exception
handlers before conditional reference, long and boolean final assignments.
Source and frozen provider profiles each run 56 fresh JVM cases with two
attempts through typed, frozen, reflection and method-handle callers. The
observer checks partial preparation, null defaults, wide snapshots, callbacks
that read the assigned fields, original and replacement failures, and cached
initialization success or failure. Full inventories, metadata and Exceptions
remain unchanged. Seven independently compiled changes to catches, defaults,
conditions, snapshots and publication order must fail the immutable observer.

`intersection_void_invocations` uses genuine Kotlin 2.0.21 and javac/D8 originals.
The Kotlin caller copies this, casts to the first interface, and calls an
unchanged generic method bounded by two interfaces. Source and frozen provider
modes each run 36 fresh JVM cases through direct, frozen, reflected and
method-handle routes, including class initialization, exact failures and later
callee casts. Reflection supplies first-interface-only and null values directly
to the provider so its effects before a later failed cast remain observable.
A negative class changes only the implemented second interface, retaining every
method's Code and descriptor: the original still reaches its later failed cast,
while recovery must reject the missing nominal witness. Complete Kotlin
Metadata, Java Signatures, Exceptions and class/method inventories remain exact.

`erased_boolean_invocations` retains javac 8/D8 originals for a public instance
method called through an inheriting child using its Object-bounded formal
directly and a static method using an
invariant callback with a closed interface bound. Source and frozen-provider
profiles each compare 68 fresh JVM cases through direct, frozen, reflected and
method-handle callers. True/false branches, nulls, exact failure identity,
first/repeated initialization failures, long extrema, signed zero and NaN bits
remain observable. The callback's original checked result must fail after its
read and before later effects. Complete original metadata and runtime inventories
are retained. Same-Code wildcard, extra and unused-formal contracts are rejected;
independent javac rejects an invalid bound, and an independently compiled inverted
boolean result must fail the immutable observer.

`erased_pair_invocations` retains javac 8/D8 originals with two independent
Object-bounded method formals, an invariant two-parameter transform, a callback
on the output formal and a result container on the input formal. Inherited
virtual calls, interface calls and the provider's original overload delegation
share the full original contracts. Source and frozen-provider/interface modes
each run 112 fresh JVM cases through direct, frozen, reflected and method-handle
routes, checking both wide extrema, input/output identity, null receivers and
callbacks, initialization failures and the callback's original late checked
bridge. All class/method, Signature, Exceptions and helper inventories remain
exact. Same-Code contravariant, extra and unused-formal declarations are rejected;
independent javac rejects the inconsistent overload, and a separately compiled
double-callback implementation must fail the immutable observer.
Native direct/linked cases additionally exclude unrelated SDK methods with the
same descriptor and unresolved nested types, while requiring failure when an
original call actually names that same unsupported declaration.

`erased_factory_invocations` retains javac 8/D8 originals for a public generic
static factory inherited through a two-class facade over a package-private
provider. Its covariant callback and invariant container result share one
Object-bounded method formal. Source and immutable dependency modes each run
36 fresh JVM cases through four original caller routes. They check erased
payload identity, wide flags, the declaring class's initialization without
facade initialization, first/repeated initialization failures and exact callback
failure order. Complete metadata, Exceptions and inventories remain unchanged.
Same-Code contravariant and extra-formal variants must fail recovery; javac also
rejects the contravariant provider implementation. A duplicated-callback
implementation must fail the unchanged runtime oracle.

`allocation_field_prefix` uses unchanged javac 8/D8 originals with a field read,
a static call that overwrites its input register, a checked result and nested
construction. Its getter can replace the field before the second read. Source
and dependency-provider modes each compare 72 fresh JVM cases through direct,
frozen, reflection and method-handle callers: allocation/class-initialization
order, first/repeated initialization failures, exact exception identity, null
and failed casts, mutable fields, fresh nested objects and retained input
metadata. An independently compiled cached-field variant must fail the oracle.

`conditional_allocation` uses javac 8/D8 originals with allocation before a wide
argument call, a checked reference argument and a conditional repeated getter.
The getter returns different objects on its first and second calls; the second
can also become null. All-input and frozen Provider-dependency profiles each
run 76 fresh JVM cases through direct, frozen, reflected and method-handle
callers. They check class-init order, first/repeated initialization failures,
null providers and owners, failed casts, original exception and object identity,
long extrema and both branch outcomes. SDK disassembly independently verifies
the actual allocation, cast, conditional branch and two getter invocations.
Rebuilt Java 14 source retains the original declarations and metadata, with
all generated helpers accounted for. Independently caching the getter inside
the constructor arguments must fail the oracle. Native cases verify wide and
reference first arguments and reject receiver use, bypasses, cycles, missing
branch values, outside entries and inaccessible initializers.

`conditional_scalar_conversions` retains the original new expression around a
nullable branch, float-to-double conversion, `Math.ceil`, double-to-int conversion
and later argument call. Source and frozen provider/target profiles compare 72
fresh JVM cases with two attempts, numeric edge values, original object identity,
class initialization and failure order, complete inventories and metadata, plus
four independently compiled conversion/order counterexamples.

`conditional_array_allocation` uses unchanged javac 8/D8 bodies with an integer
sign branch and a reference-array load inside the first constructor argument.
Source and frozen Provider/Target profiles each compare 60 fresh JVM cases with
two attempts through ordinary, frozen, reflected and method-handle callers.
The observer verifies both branches, distinct repeated index reads, array and
element aliases, integer extrema, nulls, bounds failures, exact callback failure
identity and first/repeated class-init failures. Rebuilt Java 14 preserves all
original metadata and runtime entries. Independently hoisted arguments, a
cached index and an eager array getter must fail the frozen observer. Native
cases cover all four signed if-zero relations and reference/multidimensional
arrays while retaining receiver, type, forward-edge, entry and access guards.

`allocation_static_integer` uses unchanged javac 8/D8 bodies with a virtual
getter followed by a static integer read in the original constructor arguments.
Source and frozen Provider/Fields/Target profiles each compare 36 fresh JVM
cases with two attempts through ordinary, frozen, reflected and method-handle
callers. They retain getter/class-init order, integer extrema, live field
snapshots, reference aliases and original failures, including first/repeated
class-init failures. All source metadata and runtime entries are audited.
Independently hoisted or eager field reads and late integer/reference rereads
must fail the frozen observer. Native cases also cover encoded mutable and
final values, input-register reuse, range calls, SDK declarations and ordering,
while malformed types, static kinds, frames and receiver ownership still fail.
Its `--field-first` profile reverses constructor parameter/effect order before
original compilation, checking that lookahead selects the same field family as
the graph. Source and frozen profiles retain the earlier field snapshot even
when the later getter rewrites the field.

`allocation_integer_arithmetic` uses unchanged javac 8/D8 bodies with register
and literal integer additions/subtractions after allocation. Source and frozen
Target profiles each compare 64 fresh JVM cases with two attempts through
ordinary, frozen, reflected and method-handle callers. Signed overflow,
extrema, literal signs and incoming primitive snapshots survive reentrant
class-init mutations and first/repeated initialization or constructor failures.
Full metadata and runtime inventories remain unchanged. Independently checked
overflow, late input reads and a wrong literal sign must fail the frozen oracle.
Native cases cover register, two-address, literal and range forms; division,
remainder, incompatible words, receiver aliases, entries, handlers and unknown
opcode forms retain clear rejection.


`nullable_array_loop` uses unchanged javac 8/D8 code with a nullable temporary
array followed by a reference load/call/store loop. Source and frozen Provider
profiles each exercise 32 fresh JVM cases with two attempts through direct,
frozen, reflected and method-handle callers. Null and empty inputs, element
identity, visit counts, exact failures and the committed output prefix retain
their original behavior. Full metadata/runtime inventories and immutable input
hashes are checked. Skip-last, repeated-first and early-copy mutations must fail
the frozen oracle. Native cases cover every primitive component, references,
multidimensional arrays and opposite predecessor orders; missing, conflicting,
wrong-width and out-of-frame evidence remains rejected.

`contextual_member_names` uses unchanged javac 8/D8 classes in a `yield` package
with original `yield`, `record`, `sealed`, `permits` and `var` member names.
Source and frozen Base profiles each compare 32 fresh JVM cases with two
attempts through direct/frozen/reflected/method-handle callers on Java 8 and
Java 21 generated classes. Original member reflection, inherited and superclass
dispatch, overflow, null receivers and exact failure identity remain intact.
Full metadata/runtime inventories and all frozen input hashes are checked.
Dropped superclass behavior, replacing a method call with a field and incorrect
arithmetic must fail the oracle. Native cases retain type-name/type-parameter
restrictions and true keyword rejection.

`shared_else_final` uses unchanged javac 8/D8 constructor code with three
short-circuit guards sharing one else arm. A field read and callback remain
between guards; each branch stores a blank final before observing escaped this,
and a common final retains its later write. Source and frozen Trace/Value
profiles each compare 96 fresh JVM cases with two attempts through direct,
frozen, reflected and method-handle construction. Every guard combination,
guarded null failure, null choice, exact callback failure, field snapshot and
committed store is checked. Original metadata/runtime inventories and frozen
inputs are retained. Eager predicates/field reads and delayed branch/common
stores must fail the oracle. Native cases include static initialization, shared
field/package name collisions and rejected ownership/control/type mutations.

The static-super-accessor runner's `--integer-argument` profile compiles an
original Kotlin forwarder carrying an integer after its receiver. Source and
frozen superclass profiles each exercise 40 fresh JVM cases with two attempts,
including integer extrema, overridden subclass receivers, null receivers,
class-initialization mutations and exact first/repeated failures. The original
entry, parameter metadata and `(I)V` helper accounting remain checked; a changed
argument and virtual-dispatch mutation must fail the frozen observer. Native
cases include original package access, range frames and rejected layouts,
visibility changes and generic contracts.

`monitor_post_release` uses the monitor sparse-cleanup runner's `--post-release`
profile. An unchanged Kotlin/D8 original returns a synchronized expression's
boolean and performs an optional callback after releasing the lock. Forty cases
exercise direct/frozen/reflected/method-handle routes, exact before/body/after
failures, reentrancy on both sides of release, mutable identity and cross-thread
acquisition. All original metadata and runtime entries are retained in source
and frozen-payload profiles. A separately authored Java implementation moving
the callback inside the lock must fail the immutable ownership observer.


`erased_super_constructor` supplies an ordinary javac unchecked source cast
whose D8 body passes the original reference directly to its superclass. Both
source and frozen Base/Sink profiles retain exact constructor Signatures and all
runtime entries. Twenty-eight fresh JVM cases (two calls each) compare direct,
frozen, reflected and method-handle routes, partial-object identity, original
bridge failures, callback/body failures and first/repeated class-init failures.
A separately compiled wrong-overload variant must fail the immutable oracle.


`monitor_multiple_release` runs through `--multiple-release` in the sparse monitor
runner. Its unmodified Kotlin/D8 original has two early-return releases and a
third release before a conditional callback. Both source and frozen-payload
profiles compare all runtime metadata and 40 immutable-observer cases, including
return values, original exception identity, cross-thread release and reentrancy.
SDK disassembly independently retains all four exits, including cleanup; no
positive instruction or metadata patch constructs the original control flow.


`protected_allocation` uses unchanged javac 17/D8 originals whose catch returns
the original exception or reads a static fallback. Both source and frozen
payload profiles compare 40 fresh JVM cases, two attempts each, through direct,
frozen, reflected and method-handle callers. Target/fallback class initialization,
argument and constructor failures, original exception identity, mutable aliases
and complete runtime metadata are retained. SDK disassembly independently
checks allocation and argument preparation share the original catch interval.
An independently compiled argument hoist must fail the original-order oracle.


`null_constructor_arguments` retains unchanged javac 8/D8 bodies for `this`
and `super` calls with a null generic argument and a competing Object overload.
Source and frozen Owner/Sink profiles compare 64 fresh JVM cases, two attempts
each, through direct, frozen, reflected and method-handle callers. Original
argument values, mutable state and identity, initialization and postlude order,
original failures and repeated initialization failure are checked. Independent
SDK disassembly confirms the null origin and constructor target. A separately
compiled wrong-overload implementation must fail the immutable oracle, while
complete runtime declarations, metadata and generated helpers stay accounted for.


`constructor_class_literals` compiles unchanged javac 8/D8 originals whose
superclass arguments begin with a Class literal and include copied references,
wide constants and an effectful preparation call. Source and frozen Base profiles
each compare 36 fresh JVM cases with two attempts, including missing target
classes and first/repeated initialization failures. Class literals must not
initialize their target and must remain usable after target initialization has
failed. Complete runtime metadata and helpers are checked after recompilation;
a separately compiled eager-initialization variant must fail the immutable oracle.

`raw_interface_parameter_bridges` obtains an authentic checked bridge from
javac and changes only the class Signature to a raw interface edge. All method
Code bytes and descriptors remain unchanged; both compiler and encoded
originals run before recovery. Source and frozen Api/Sink profiles each compare
55 fresh JVM cases with two attempts through interface, immutable, reflected,
method-handle and concrete calls. They retain casts, full generic callback
types, mutable aliases, exception identity and effect order. Complete original
runtime entries and metadata are audited, with only bridge/synthetic flags
reported as different. Full tail metadata mutations are rejected; a separately
compiled double-call bridge must fail the unchanged observer.

`nested_carrier_invocations` retains unchanged javac 8/D8 factory and caller
bodies with nested lower-bounded callback carriers. Source and frozen-provider
profiles each compare 56 fresh JVM cases with two attempts, including nulls,
nested callback casts, aliases, original failures and repeated initialization
failure. Every original runtime entry, full Signature and generated helper is
checked after recompilation. Metadata-only changes preserve all Code bytes and
must fail when composed variance or the set of method formals loses its proof;
a separately compiled double callback must fail the immutable observer.

`mangled_interface_names` uses genuine Kotlin value-class return mangling on a
generic interface, superclass and child override. A branch interface inherits
the original declaration, and a legal source name deliberately occupies the
first alias candidate. Seventy fresh JVM cases with two attempts compare
original and recovered dispatch, nulls, mutable identity and original failures.
Compiler metadata and all runtime entries are checked against the disclosed
name mapping. External interface renaming is rejected, and a separately compiled
override that skips its superclass must fail the immutable oracle.

`captured_generic_constructor` uses Java 25 to compile captured-field writes
before a superclass call with constant, generic and wide arguments. A parent
callback observes the field before child initialization finishes. Source and
frozen-provider profiles compare 36 JVM cases with two attempts and exact
runtime inventories and metadata. For the older D8 class reader, only the class
version bytes change; both original and encoded inputs must match before
recovery, and all Code and attribute bytes remain unchanged. Independently
compiled wrong-overload and delayed-capture bodies must fail the frozen oracle.

`erased_constructor_delegations` is genuine Kotlin compiler output for a private
generic constructor and its raw public synthetic access constructor. The source
round trip retains both entries, every parameter and the unused marker, while
reporting the source compiler's synthetic-flag difference. Forty JVM cases with
two attempts compare private target effects, wide bits, aliases, nulls and exact
failures. The original forwarding Code is independently checked with the SDK;
a separately compiled repeated callback must fail the immutable observer.

`allocation_field_aliases` checks one field read and cast inside a constructor
argument program, with repeated reference aliases and raw scalar/wide register
copies. Source and frozen-provider profiles each run 32 JVM cases twice, comparing
initialization order, original failures, live aliases, NaN payloads and signed
zero. The constructor changes the original field to expose accidental rereads.
SDK disassembly and recompilation verify original code shape, inventories and
metadata; independently compiled hoisted-read, late-reread and numeric-float-copy
variants must fail the frozen observer.

`indexed_character_bridges` encodes only the bridge flags on an original final
`charAt(int)` forwarding body; Code bytes and descriptors stay unchanged. Source
and frozen interface profiles, with and without the synthetic bit, each compare
260 JVM calls and 100 expected failures through direct, interface, reflected and
method-handle callers. Unsigned character values, surrogate code units, virtual
target overrides, mutation, array access order and original exception identity
are retained. Full runtime metadata, inventories and generated helpers are
checked after recompilation. A same-Code target-formal mutation must be rejected;
a separately compiled byte-narrowing variant must fail the immutable observer.

`interface_super_getters` retains an original explicit interface-super getter
while adding only bridge/synthetic flags to its classfile. Source and frozen
provider profiles each compare 140 JVM calls and 100 expected failures through
class, two interface, reflected and method-handle callers. An immutable subclass
checks outer virtual dispatch; counters distinguish the chosen direct default
from its ancestor. Return identities, mutable arrays, failure order and original
exception identities are preserved. Full metadata and runtime inventories are
audited, a same-Code unused formal is rejected, and a separately compiled body
that bypasses the default must fail the frozen observer.

`encoded_final_defaults` uses unchanged javac/D8 output with eight explicit
typed zero values preceding original static final stores. Independent DEX
parsing checks the encoded values and SDK disassembly checks every store. Source
and frozen-trace profiles each run 30 fresh JVM cases twice, comparing reentrant
reads before and after initialization, raw floating bits, reference aliases,
ordered effects and first/repeated initialization failures. Recompiled consumers
retain their runtime field reads. Original metadata and inventories are checked,
with the existing non-inlining helper for a separate encoded constant disclosed.
A separately compiled premature final-field write must fail the frozen oracle.

`constructor_encoded_fields` clears only the final bit on two owned integer
fields before compiling callers; their original ConstantValue attributes and
all method Code remain unchanged. Source and frozen-provider profiles each run
30 fresh JVM cases twice through ordinary, immutable, reflected, method-handle
and direct-super callers. Reads before this/super calls retain class initialization,
integer and wide snapshots, later mutations, effect order and first/repeated
failures. DEX values and reads, complete runtime inventories, metadata and
Exceptions are checked; mutable source initializers replace ConstantValue
attributes. A separately compiled late-read body must fail the frozen observer.

`literal_superclass_bridges` uses real javac 8/D8 input with a bounded generic
root, a fixed argument parent, a plain middle class and an original child void
bridge. Source and frozen parent modes each run 216 fresh JVM cases with two
attempts through direct, raw-parent, reflected and method-handle calls. Frozen
child overrides, null receivers and values, wrong carrier types, 64-bit stamps,
input mutations, first/repeated initialization failures and exact callback
failures must agree with an independent observer. Full original inventories,
metadata and Exceptions are retained apart from reported helpers. Four compiled
effect, width and identity mutants must fail the same immutable observer.

`final_instance_names` uses genuine Kotlin 2.0.21 final public instance methods,
with synthetic and ordinary source profiles and an unchanged same-named owned
dependency. Source and frozen base modes each run 96 fresh JVM cases with two
attempts through direct, frozen, reflected and method-handle calls. The observer
checks original values/identity, ordered mutations, null/type failures, callback
identity and first/repeated initialization failures. Name and synthetic flag
changes and the three/four synthetic/ordinary D8 CLASS annotation-loss sites
are disclosed;
other metadata and full inventories remain exact. The same frozen observer must
reject three compiled callback/result mutants, and an unchanged Code private
method variant must be rejected by recovery.

The `allocation_super_calls --protected-parent` profiles place the inherited
protected methods in a separate ancestor package. Source and frozen parent/target
runs retain lexical superclass dispatch, allocation and initialization order,
reference and integer snapshots, original failures, every declaration and
metadata through 208 fresh JVM cases with two attempts. The same five compiled
counterexamples distinguish virtual dispatch, early/repeated calls and late
result reads.

The final instance name profiles also exercise implicit finality from the class
and complete generic owner/result contracts. Ordinary and synthetic Kotlin
methods keep their original final or nonfinal flags, class/method Signatures,
metadata and callback bodies through source and frozen parent profiles. Each
runs 96 fresh JVM cases with two attempts, a private-access counterexample and
three independently compiled effect/result/callback controls.

`allocation_scalar_conversions --unary` adds genuine javac/D8 negation of int,
long, float and double and complement of int/long before the existing six scalar
conversions. Source and frozen provider runs cover 120 fresh JVM cases with two
attempts, raw NaN/signed-zero bits, infinities, integer minima, field versions,
nulls, initialization failures, protected cleanup and monitor ownership. Seven
compiled mutation controls and complete metadata/inventory checks remain active.

`guarded_delegation_chains --guarded-primary` compiles an independent Kotlin
super argument with a real guard and getter before the primary's final stores.
The untouched compiler/D8 output retains the default constructor and overload
chain. Source and frozen SDK-parent profiles compile every recovered Java file
at release 25 and compare all original entries, metadata and Exceptions against
the disclosed synthetic-constructor flag receipt. Each profile runs 256 fresh
JVM cases with three input variants through direct, frozen, reflected and
method-handle callers. Original values, NaN payloads, signed zero, null checks,
getter order, exception identity and fields observed before/after superclass
publication must match. Independently compiled wrong masks, reversed branches,
duplicate guards, late argument evaluation and early parent stores must fail
the frozen observer; a separate correct Java control must pass.


`paired_void_carriers --callback-tail` independently compiles a binary consumer
with two contravariant method-formal inputs and one fixed Object result slot.
The original direct seed occupies the second callback input, so frozen generic
reflection and opaque input identities expose a swapped binding. Source and
frozen provider profiles each check 112 fresh JVM cases with two attempts,
including virtual dispatch, initialization failures, nulls, callback failures,
array mutation and the original later result cast. Every recovered Java file
compiles, all entries/metadata/Exceptions match, four same-Code Signature
counterexamples are rejected, and five compiled wrong programs must fail the
immutable observer. Recovered output is never used to author the original.
