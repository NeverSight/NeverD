# highir shared indirect call targets

## Ownership and behavior

HighIR retains a computed indirect-call target when guards or arguments
also use that SSA value. Existing resolution still decides callable identity;
the call no longer forces duplicate arithmetic expansion. Memory-read target
handling retains its separate reaching-value proof.

## Reproduction and validation

`HighSourceExpressions.GuardAndCallShareTheComputedTarget` constructs a
public MedIR fixture with 32-bit XOR, zero extension, wrapping address addition,
a null guard and an indirect call that observes its actual target argument.
Generated C executes null and callback cases at O0/O2. Run
`NeverDHighControlFlowTests` and `NeverDMedCachedCallTargetTests`.
