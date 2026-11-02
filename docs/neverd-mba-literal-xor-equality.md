# mba literal xor equality

## Ownership and behavior

Literal XOR is invertible in fixed-width equality: `(x^k)==c` becomes
`x==(c^k)`, including flattened XOR operands and negated equality. The shared
MBA owner charges existing work/storage allowances; AND, OR and ordered
comparisons do not acquire this rule.

## Reproduction and validation

`SymConditionalControls.LiteralXorEqualityUsesTheDecodedConstant` checks
1–257-bit equality and inequality against independent APInt evaluation.
`MaskedEqualityIsNotAnInvertibleXor` exhausts neighboring AND/OR byte inputs.
Run `NeverDSymConditionalControlTests` and `NeverDSymbolicTests`.
