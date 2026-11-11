# Opaque result formal with a reifiable carrier bound

A public virtual `<T extends Carrier<?>> T get(String)` retains its complete
known unary carrier bound. Its closed nominal key supplies no method-formal
binding and predicts no concrete returned type. A separately compiled future
subclass overrides the entry, runs the original provider and may replace its
result or throw after the provider's effects. Raw, typed, discarded and later
narrowed callers must retain that dispatch and the actual opaque object.

Source and SDK profiles compile every recovered Java 8 unit and compare all
original definitions, metadata, Exceptions and helper roles. Each checks 64
fresh JVM cases twice, including null keys/results, mutable callbacks, partial
fields, exact exception/Error identity and later subtype/field checks.
Signature-only controls preserve Code; independent wrong programs must fail
against one frozen caller. Only authored fixture code executes.
