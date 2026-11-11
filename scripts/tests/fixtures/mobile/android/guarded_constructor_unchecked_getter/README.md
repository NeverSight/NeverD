# Unchecked factory before constructor delegation

Independently authored `fixture.*` constructors retain an original generic
varargs factory before `this(Object)` or `super(Object)`. Its own declared
Failure extends RuntimeException. Only the canonical closed unchecked hierarchy
proof permits the getter; checked, mixed, missing or generic throws stay excluded.
The original Exceptions attributes and this/super delegation remain intact.
The array-allocating this prefix retains terminal delegation; linear this and
super paths retain their original continuation effects.

Source and SDK profiles compile every emitted unit at the reported Java release.
Observers retain original input/token/array references, fresh literal arrays,
opaque returned values, constructor/final-store order, callback mutation and exact
failure identity. Independent wrong programs run before recovery. Metadata-only
controls preserve method Code and descriptors while changing exception contracts.
Private application code is never part of these executable fixtures.

The this-overload token parameters use descriptor-only Class inputs; the factory
retains its complete Class<T>/T Signature. The super paths also retain Class<?>
input metadata. Parameterized sibling-constructor applicability is separate.
