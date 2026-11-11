# Scalar arguments to an unseeded binary factory

An independently authored package owner exposes its public generic factory
through a public facade. The factory retains two independent Object formals in
its invariant Map result and one original int argument. Frozen callers observe
capacity bit patterns, argument capture before class initialization, provider
mutation and exceptions, late key/value casts and mutable/immutable result
identity. Class initialization and method callbacks change the source capacity
field, so replacing the captured argument with a later field read must fail.
