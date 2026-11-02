# highir common terminal continuations

## Ownership and behavior

HighIR can share equivalent terminal continuations after an optional prefix.
Matching checks types, operands, memory ordering and actual call targets; only
temporary names confined to the copies can differ. Reads stay after preceding
calls, and jump entries and exception wrappers prevent an unsafe merge. A
function with exception flow is left unchanged, as for structured continuation
folding: the unwinder enters catch continuations, landing pads and dispatcher
entries without a goto, and the matched calls may unwind to different pads.

## Reproduction and validation

`HighSourceExpressions.CommonTerminalTail*` executes original and folded
source at O0/O2 against a helper that changes the subsequently read memory.
Negative cases cover call targets, operands, ordered accesses, escaping names,
labels, unknown values and exception wrappers.
`CommonTerminalTailKeepsExceptionEntries` places a catch continuation, a
language table and a dispatcher entry on the matched arm. Run
`NeverDHighControlFlowTests` and `NeverDWindowsEHSourceTests`.
