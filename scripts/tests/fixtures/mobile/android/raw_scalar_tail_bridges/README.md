This owned Java 8 program implements a raw generic API with an ordinary erased
entry and a differently named scalar target. The entry calls an effectful final
carrier getter and forwards a fully specified `Sink<? super Token>` tail. The
runner changes only that entry's bridge/synthetic access bits, verifies all Code
attributes remain identical, and executes both forms before D8 and NeverD.

The frozen callers use interface, precompiled, reflective, method handle and
direct dispatch. Each fresh process makes two calls and checks scalar bits,
getter effects, mutable token aliases, callback changes to the actual returned
reference, exception identity and ordering. Null receivers, bad casts, null
tails, each throwing stage, callback bridge failures and an `Error` are observed.
Source and SDK carrier declarations, and 32/64 bit conversions, are separate
profiles. Every emitted Java file must compile and every original definition,
generic signature and declared exception is compared; only the reported
bridge/synthetic source flag difference is allowed. Independent wrong programs
must be caught by these same immutable observers.
