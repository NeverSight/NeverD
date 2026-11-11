# Independent direct pair seeds

Owned Java 8 declarations use two independent method formals as direct
inputs and as distinct invariant result slots on a generic factory owner.
Class formals F/S remain separate from the static method's A/B scope.

The provider returns an independently selected raw pair, whose fields may
contain arbitrary values. Frozen raw, typed and method-handle callers verify
that the binding infers neither result identity nor element contents. They
also compare callback order, exact checked/Error identity, late field casts,
nulls and result snapshots after callback state changes. Source and SDK
profiles retain all original definitions, full Signatures and Exceptions.
