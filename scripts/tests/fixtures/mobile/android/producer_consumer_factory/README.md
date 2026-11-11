# Producer and consumer carriers with an invariant result

Owned Java calls a public inherited static method on a package-private provider.
One Object-bounded method formal appears covariantly in Iterable, contravariantly
in Comparator, and invariantly in List. The original call, iterator, callback,
later reads and actual result remain observable; no sorting result is inferred.

The frozen observer checks reference identities, nulls, aliases, iterator reads,
callback order, mutation between reads, result mutation, and exact checked,
unchecked, Error and throw-null failures through three invocation routes.
Ten wrong programs must fail before NeverD. Source and SDK declaration profiles
inventory all original definitions, compile every Java unit, and compare complete
compiler metadata and Exceptions except reported source helpers. Native cases
also reject incomplete, foreign, bounded or wrongly variant formal relations.
