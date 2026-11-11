# Generic reference result after wide-carrier calls

This independently authored variant reuses the two-argument Function, carriers
and tracing class in `value_carrier_bridges`. The primitive target retains its
complete `Box<CarrierA>` return Signature and returns an existing box or null.
The bridge preserves the original two virtual unboxing calls and target result.

The oracle compares 32 wide values, 29 identities and ten ordered failures, then
checks unchanged generic target reflection. Changing only the target return
Signature to `Box<CarrierB>`, with identical Code and runtime descriptors, must
fail before any recovery output is published.
