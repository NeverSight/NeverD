# Raw factory constructor delegation

Owned Java 8 final generic class implements a generic marker. Its no-argument constructor obtains a raw unary carrier from a static generic factory and delegates to a same-class constructor whose parameter retains the original owner formal invariantly or covariantly. A generic marker constructor provides a competing runtime overload. Frozen callers observe factory initialization/effects, exact input-carrier identity, mutation, partial final fields, nulls, opaque element values, repeated allocations and failures at each boundary.

Source and SDK-input profiles compare every original definition, metadata and Exceptions, compile all emitted Java, and use four call routes and Signature-only controls. Constructor binding does not predict the factory result or permit uninitialized-this escape.
