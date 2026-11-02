# Wide integer arguments without a C prototype

HighC's fallback call rendering must retain an integer argument's complete
machine carrier when no declared parameter type performs its conversion.
For example, a 64-bit all-ones argument printed as a bare `-1` has C type `int`.
In a Windows x64 unprototyped indirect call, the observed callee can receive
`0xFFFFFFFF` instead of `0xFFFFFFFFFFFFFFFF`.

The fallback explicitly selects the wide integer carrier and renders the
complete original argument. It must not select only the innermost literal:
zero-extending a 32-bit all-ones value and sign-extending it have different
64-bit results. Pointer arguments, floating arguments and declared parameter
types keep their existing conversion paths.

`HighCIntegerConversion.UnprototypedImmediatesKeepTheirCompleteWord` constructs
public HighIR and executes the generated C at O0/O2 against a callback that
observes its full 64-bit argument. It checks zero, one, signed/unsigned 32-bit
boundaries, values above 32 bits, signed/unsigned 64-bit all-ones, zero/sign
extension and an explicit unsigned widening cast. The regression fails with
the previous immediate-unwrapping behavior. It requires neither an external
source interface nor a private input binary.

Run `NeverDHighControlFlowTests` and the HighC forwarding, Windows EH source
and symbolic-bridge owners after changing this conversion boundary. This rule
belongs to HighC's untyped argument fallback; typed function signatures and
LLVM call types keep their separate declared-parameter conversions.
