# Scalar allocation field copies

Independently authored Java reads a volatile boolean after allocating a target,
then retains two aliases for a range constructor and one live value afterward.
The target's first initialization toggles the source fields; its constructor
publishes partial final fields and mutates the source again. Frozen observers
check both instance and static fields, class initialization failure, null
precedence, alias identity, raw float/double bits and exact exceptions.
Only these owned fixtures are executed.
