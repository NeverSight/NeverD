# Scoped superclass scalar suffix

Owned generic constructors forward only their direct seed and complete invariant
unary carrier to the original parent constructor. The caller's extra float input
is used in its original continuation. Constructor formals remain independent of
the parent's recursive class formal; the extra input contributes no binding.

The frozen observer retains parent defaults, input and callback identity,
snapshot results, partial construction, exact failures and the extra float's bit
pattern, including a NaN payload and signed zero. Source and dependency-parent
profiles audit all original declarations and compiler metadata. Wrong programs
must fail before recovery; only owned Java is executed.
