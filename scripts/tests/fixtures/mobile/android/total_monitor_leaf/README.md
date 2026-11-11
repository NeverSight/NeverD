# Total monitor leaf

Owned Java 8 declarations exercise monitor regions with a single release and
no potentially throwing operation after acquisition. Synchronized methods
that lock the incoming receiver throughout the total body also retain their
original method modifiers without an explicit reentrant acquisition.

Frozen direct, typed and method-handle callers compare returned identity,
arbitrary raw values, null acquisition failures, late casts and integer
overflow. Controlled lock contention verifies the acquired object and release;
the observer checks the exact blocked lock and owner, with bounded waits.
Source and SDK-provider modes compare all original definitions and metadata.
Counterexamples must fail the same observer.
