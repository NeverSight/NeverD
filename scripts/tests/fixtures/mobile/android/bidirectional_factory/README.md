Independently authored bidirectional callback factory with one fixed nominal bound. The factory retains arbitrary provider behavior and returns an existing or null carrier. Frozen callers and observers validate both callback identities, directions, partial writes, mutations and failures.

The contextual variant gives both result formals an Object bound. Its forward
callback also accepts a fixed context, while the reverse callback exchanges the
two formals. Original context identity and null, mutable input, callback order,
partial fields, arbitrary intermediate objects and returned carriers are
observed by frozen callers. The compiler-generated inputs and observers remain
immutable during recovery, and five wrong programs fail the observers.
