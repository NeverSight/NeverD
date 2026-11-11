# Two formals, a property, an evaluator and the original array

Authored fixed-nominal factory binds its target and array component through an
invariant binary property and matching unary evaluator. The frozen observer
checks callback order, exact array identity, covariance failures, partial
stores, actual captured result and discarded calls. The dependency profile
supplies the original provider as declarations during recovery and as authored
bytecode during JVM verification; application APK code is never executed.
