# Bounded arrays selected by an invariant token

Independently authored static calls retain one interface-bounded method formal,
two fixed nominal context inputs and an invariant token. The actual opaque array
keeps its original interface-array cast. Frozen future context and trace
implementations check callback order, mutation, exact failure identity, result
caching and the caller's later subtype-array cast, including discarded results.
Source and SDK profiles compile every generated Java unit and audit all original
definitions, metadata, Exceptions and generated helper roles.

The `--carrier-result` profile returns the actual invariant `List<T>` through
that same interface bound and token, alongside two fixed contexts. Frozen
callers retain list identity and perform subtype element checks only after the
original caller has returned. Source and SDK profiles reject unbound, variant,
raw or unrelated result contracts without traversing or copying list elements.
