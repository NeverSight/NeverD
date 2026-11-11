# Repeated dependent collection seeds and a lower callback

Independently authored Java8 declarations retain two distinct arguments of the
same T extends Collection<E>, a separate Object-bounded E, and a callback<? super
E>. The provider's opaque result, partial effects, input/target identities,
callback and mutation order, null/alias cases and delayed List casts are observed
in fresh JVMs. Every recovered unit compiles in source and SDK modes; original
metadata, roles, Exceptions and generated helpers are compared. Private
application bytes and behavior are excluded from these fixtures.
