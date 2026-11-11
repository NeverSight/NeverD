# Invariant inputs and independent nominal results

The owned virtual provider declares `<T> Provider bind(String, Class<T>,
Predicate<T>)`. Complete invariant input declarations bind the same Object
formal; the closed result has no generic relation to either input or receiver.
Source and SDK profiles preserve original signatures, flags, metadata,
exceptions, frozen observers and DEX definitions. Native cases include static
nested owners, inherited virtual dispatch, used/ignored results and an
independent result owner.

Fresh JVM observers check exact receivers and arguments, predicate bridge
casts, virtual overrides, boolean stores, actual result identity, live fields,
result snapshots, mutable callback effects and exact checked/Error identity.
Null, different-receiver and self results are all exercised without predicting
provider behavior. Deliberately wrong programs must fail the observer.
