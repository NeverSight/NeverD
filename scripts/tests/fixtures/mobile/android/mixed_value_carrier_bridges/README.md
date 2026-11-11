# Mixed reference and wide-carrier bridge

This independently authored three-argument variant reuses the carrier and tracing
classes in `value_carrier_bridges`. The first two checked references flow unchanged
to the primitive target; only the third argument is unboxed. The target returns
the first reference unchanged. The runner edits only class Signature and flags.

The oracle compares sixteen wide values, sixty identities and nine ordered
failures. It verifies that nullable reference arguments retain their values,
unboxing does not clobber them, direct primitive calls do not unbox, and neither
the first carrier's unboxing failure nor the boxing factory's failure is reached.
