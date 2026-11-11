# Floating constructor prefixes

Owned Java8 constructors compute add/subtract/multiply/divide/remainder results
for float and double before delegating, including a chained prefix with explicit
float rounding boundaries. Original D8 must witness all ten arithmetic families
and both address forms. The getter snapshot and partial final-field stores are
observed through direct, reflection and two MethodHandle routes.

Source/SDK profiles compile every emitted Java25 unit, preserve all original
definitions and complete metadata/Exceptions, and compare64 fresh JVM cases
twice. Frozen original bytecode supplies numeric outputs; NaN payloads from
arithmetic are normalized, while signed zero, infinities and finite result bits
remain exact. Fourteen separate wrong programs must change that oracle or fail
its state/identity assertions. No private input or generated private Java runs.
