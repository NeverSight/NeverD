# A bounded carrier with a nominal input

Owned Kotlin uses a private method-owned T bounded by a public static nested
Node class. Its invariant Element<T> carrier has the same complete declaration
bound, while the second parameter is the nominal Node rather than T. The erased
call must retain the original nullable cast check and virtual update operation;
it cannot infer a narrower argument class from a carrier or a runtime node.

The frozen observer compares a raw carrier accepting every Node with a typed
carrier whose JVM bridge rejects a different Node subtype. It checks null input
before null receiver, receiver and argument effects, exact checked/unchecked
errors and partial state. Both source and SDK-carrier profiles compile every
emitted Java unit and account for every original identity and type-variable
owner. Original bytecode and observers are frozen before the product fix.
