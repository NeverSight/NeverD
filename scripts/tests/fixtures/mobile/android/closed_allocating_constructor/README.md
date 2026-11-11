# Closed constructor delegation after allocation

An independently authored private constructor first allocates a generic box
from a wide argument, then delegates with a callback whose complete arguments
are fixed nominal types. Frozen ordinary, reflection and method-handle callers
observe initialization, sticky failures, partial final fields, null callbacks,
wide extrema, callback arguments and original result or exception identity.
All compiler-generated inputs and observers remain immutable during recovery.
