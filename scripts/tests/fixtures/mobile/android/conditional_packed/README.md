# Conditional comparison and packed long allocation arguments

An independently authored Java 8 expression allocates before a seed-provider
call, compares two original inputs, packs raw float bits using long shift/mask
operations, calls a tail provider and initializes its target. Reference equality
and ordered integer comparisons each run with owned/SDK Provider and Target.
Frozen direct, compiled, reflection and MethodHandle callers compare both
branches, reference identity, NaN payloads, signed zeros, shifts with masked
distances, partial effects and exact class-init/provider/constructor failures.
All Java, original entries, metadata and Exceptions remain checked. Independently
compiled wrong programs change the condition, bit operations, seed or effect.
