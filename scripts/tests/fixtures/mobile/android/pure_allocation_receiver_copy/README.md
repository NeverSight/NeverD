# Pure allocation receiver copies

The Java files specify an independently executable constructor call. The owned
smali client expresses that same operation with explicit pure register copies,
a different contiguous constructor receiver, and reuse of the allocation word
as a wide local after initialization. It contains no field read, call or handler
between allocation and construction. No compiler output is rewritten, and the
runner does not claim that D8 chose this register layout.

Original Java Target/Trace classes and frozen callers are the runtime oracle.
The runner compares scalar values, one returned/partially published object,
constructor counts, final fields and ordered exception identity, and matches
all original client metadata. Four wrong Java programs must fail the observers.
The chained/early variant adds another receiver alias before argument copies.
Native cases additionally cover owned constructor declarations, input ordering,
wide values and rejected uninitialized consumers or lost origins.

`--conditional-preparation` independently authors a boolean branch and wide
negation before the final receiver copy. Its Java specification and frozen
observer are authored before compilation. `--inverse-condition` exercises the
other branch polarity. These profiles require an unchanged allocation identity
through the final pure range adapter and reuse of the old allocation word after
initialization. They do not claim the smali was emitted by a compiler.
