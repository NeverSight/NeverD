# highir guarded address bases

## Ownership and behavior

For a stable full-width input, the guard `q!=s` also tests the decoded base
`(q^k)-(s^k)` against zero. HighIR may name that base when a directly guarded
read acquires a shorter field displacement. The read keeps its original
execution point; no runtime pointer value, object layout or call identity is
inferred. Relocations, narrowing, mutable inputs and incoming labels refuse
the optional rewrite, whose scan shares an input-sized function allowance.

## Reproduction and validation

`HighSourceExpressions.GuardedAddress*` executes before/after C at O0/O2
for both equality arms, null encodings and field loads. Counterexamples cover
reassignment, relocation constants, truncation, distinct inputs, incoming
labels and displacements that cannot improve. Run `NeverDHighControlFlowTests`
and `NeverDWindowsEHSourceTests`.
