# Allocation field receiver transfer

Authored range-constructor fixture keeps a field receiver above the compact
field instruction's register range. Its original field read precedes reuse of
a preparation register for a scalar argument. The frozen observer checks class
initialization before the volatile read, exact argument and returned identity,
constructor callbacks, escaped partial objects and exact failures. Unused
parameter words keep the register pressure explicit and reproducible.

D8 emits a checked register layout. The runner preserves that DEX, then applies
an explicit alpha-renaming of two temporary versions: the allocation uses the
range-call receiver directly and its redundant copy becomes nop; the field
receiver uses the later scalar argument temporary. Parameters, definitions,
references, instruction boundaries, effects and all other regions stay intact.
The receipt records both sequences, and the original class files remain frozen.
