# Zero-argument result carrier bridge

This authored raw Function implementation delegates once to apply() -> long
and boxes through the original CarrierB.wrap factory. Only its interface
Signature and invoke bridge/synthetic flags change in the owned input.
The original Code, runtime descriptors and every other declaration remain.

The JVM oracle checks wide edge values, independent factory allocations,
typed calls without boxing, target failure before the factory, factory failure
identity and null receivers. Changing only the implemented result to String
must be rejected even though the original body remains unchanged.
