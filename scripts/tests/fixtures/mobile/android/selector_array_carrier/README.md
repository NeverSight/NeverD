# Selector arrays with a closed key contract

An owned static varargs factory accepts an array of binary selectors. Each
selector consumes the method-owned Object-bounded T and produces a complete
Key<?> contract; the result is an invariant Result<T>. Passing the existing
array is fixed arity and must preserve its component type, identity and writes.
The closed star argument contributes no additional inferred method formal.

The independent observer checks raw and typed selector bridges, the original
key and result casts, delayed result dispatch, array identity and mutations,
nulls, bounds errors, checked and unchecked failures, and partial effects.
Eight wrong programs must fail before invoking NeverD. Source and SDK-provider
profiles compile every emitted Java unit, account for every original class and
method identity, and compare complete compiler metadata and Exceptions.
