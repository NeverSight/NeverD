# Static access through a protected receiver

Owned Java bytecode calls a final protected generic parent method through an
explicit Child parameter in a static Child method, across runtime packages.
The original and copied-register routes must retain that receiver and the full
method-owned interface bound. No instance this exists in either caller.

The observer checks three receiver identities, arbitrary and aliased input and
result implementations, null arguments and results, a null receiver, supplied
error identity, null-throw behavior and partially committed receiver fields.
Wrong replacements must fail before decompilation. Source and SDK-provider
profiles compare every original definition, Signature and Exceptions attribute,
compile every emitted Java unit and run the same frozen observer. Only explicitly
reported source helper definitions are accepted as additional methods.
