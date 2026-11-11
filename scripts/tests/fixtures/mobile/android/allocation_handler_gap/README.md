# Allocation exception adapters

An independently compiled six-argument Java constructor call makes D8 emit
unprotected argument copies between `new-instance` and `invoke-direct/range`.
Its two ordered catch regions use distinct copy adapters that converge on the
same cleanup body. The runner requires this original D8 layout explicitly;
no input bytecode or metadata is rewritten.

Frozen direct, precompiled, reflection and method-handle callers compare the
original and recovered classes. They observe every argument identity, nulls,
partial constructor publication, final fields, caught and uncaught exception
identity, and the exact effect trace. Four independently wrong programs must
be detected. The dependency variant borrows the constructor declaration while
recovering the original client and trace definitions.

Native tests additionally reject a different same-typed input, changed catch
types, malformed exception entries, cycles, throwing adapter steps, lost
handlers and reads of an uninitialized object. Wide copies retain both words.

The `--receiver-copy` route uses an independently authored smali client. It
copies one uninitialized object into a different contiguous constructor
receiver word, keeps the original register live after construction, and uses
separate allocation/constructor catch adapters. Original Java Node/Trace
classes and frozen callers remain the runtime oracle; this route explicitly
does not claim that D8 chose this receiver alias layout.

The `--long-cleanup` route compiles an owned catch continuation with 160 ordered
callbacks and a finally body. It keeps the resulting D8 code unchanged and
requires its full long instruction sequence. Frozen direct, precompiled,
reflective and method-handle callers cover early and late cleanup failures,
errors, null arguments, original partial fields and exactly-once finally
execution. All sources compile and all original metadata is compared in both
source and declaration-only constructor modes. Nine wrong programs expose
missing or reordered steps, wrong late arguments, lost finally effects and
swallowed failures, in addition to the existing constructor counterexamples.

The `--entry-frame` authored smali variant saves an incoming value for its
constructor argument, then overwrites the original input word. Allocation
failure cleanup observes the committed incoming frame; constructor cleanup
restores the saved identity. The same frozen observer checks all six original
argument identities, both partial-construction failure sites and uncaught
exceptions. Same-typed changed cleanup inputs remain native negative cases.
