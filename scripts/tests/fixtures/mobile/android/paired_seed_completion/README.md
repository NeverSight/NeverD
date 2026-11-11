# Two ordered inputs and a nested completion callback

The provider retains three independent method formals: two direct input slots
and one completion element. Its four-slot callback consumes those same inputs
and the exact nested completion, with an opaque Object result. A public facade
inherits the static declaration through a package class and its package parent.

All declarations are compiled from owned Java. Frozen direct, reflective and
method-handle callers observe argument identity, opaque result aliases, deferred
completion, reentry and exact failures in source and external-provider layouts.
Original metadata, every class and method, and all emitted Java are verified.
