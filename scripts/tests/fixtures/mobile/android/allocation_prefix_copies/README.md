# Register copies inside constructor allocations

Owned Java8 constructors allocate two distinct wrappers around two distinct original
inputs before delegating to a primary constructor. Six double padding inputs force
genuine D8 register copies between each new-instance and its initializer. Ten
input types cover every primitive width, raw float/double NaNs and signed zeros,
long boundaries, nulls, arrays and reference identity.

Source and SDK-provider profiles compile every generated Java unit and compare
64 fresh JVM cases twice across direct, reflection and two MethodHandle routes.
Frozen callers observe both allocation identities, partially assigned final
fields and exact runtime/error failures. Fourteen wrong programs must fail.
All original definitions, declaration metadata and Exceptions stay exact.
