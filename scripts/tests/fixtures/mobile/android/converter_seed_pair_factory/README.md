# Converter and two seed values

Original Java declarations anchor both invariant result formals in an invariant
converter and two direct seed inputs. Two wide values, a Boolean value, partial
constructor fields, original converter/seed identities and arbitrary returned
references are observed through frozen callers. No application data or metadata
edits are used.

The `--bounded-input` profile independently compiles a second seed of the
nominally bounded formal instead of another Object seed. Full original converter,
parameter and result metadata are reconciled with the same immutable observers.
