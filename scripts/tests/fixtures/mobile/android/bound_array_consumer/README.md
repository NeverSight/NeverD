# Bounded instance array consumer

Owned Java 8 declarations retain a public instance void call with one complete
method-formal array and an int flag. The formal has a known plain interface
bound, and its array descriptor remains the original erased bound array.

Frozen direct, explicit-type and method-handle callers verify dynamic dispatch,
array identity, arbitrary interface implementations, covariance store failures,
null and bounds failures, the original bound cast, exact checked/Error
identity, mutation before suffix failures and snapshots after callback changes.
Source and SDK profiles preserve all original definitions and metadata.
