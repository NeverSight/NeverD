# Independent key and value formals

Owned Java and Kotlin inputs exercise a two-argument carrier whose key and value
are different method-owned Object-bound formals. Both direct Kotlin standard
library facade calls and an owned static provider must preserve the full original
Signature while invoking the erased descriptor. The SDK-provider profile removes
only the owned provider definitions from DEX.

The frozen observer checks arbitrary and aliased values, null keys and values,
missing keys, standard-library default maps, exact supplied errors, and get,
containsKey, default callback, key stringification and number conversion order.
Raw maps containing Long, Double, Float and a custom Number must retain the
original Number.intValue conversion. Incorrect replacement programs are rejected
before decompilation. Every emitted Java unit is compiled; original identities,
Signatures, Exceptions and metadata are compared, with only precise D8 CLASS
annotation and Kotlin source filename differences recorded.
