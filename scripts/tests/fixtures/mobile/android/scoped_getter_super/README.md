# Scoped instance getter results in superclass constructor arguments

An original `Child<K,V,T>` constructor invokes `Builder<K,V>.fetch()` before
delegating to `Parent<K,V,T>(Node<K,V>, Path<K,V,T>[])`. Getter results are
dynamic, nullable and may differ from mutable global state. The original array
parameter stays the same object while the getter mutates its first element and
replaces the global array/builder. A later stamp call uses the original builder.

Frozen derived, caller, reflection and handle routes observe initialization,
overload selection, exact getter invocation/result snapshots, null/type/array
failures, partial superclass/subclass fields and original RuntimeException/Error.
Source and unchanged SDK declarations share identical originals/observers.
No getter body or stored node identity is inferred from its generic contract.
