# Bounded pair consumer

Independent Java sources declare two invariant input carriers sharing one Object
formal and one nominal bound. Provider reads and writes remain observable through
virtual dispatch, partial fields, wide values, nulls and exact failures. Frozen
callers and observers are compiled before DEX conversion. No application data or
compiler metadata edits are used.
