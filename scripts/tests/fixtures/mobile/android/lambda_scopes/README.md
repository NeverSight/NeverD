# Desugared lambda scope fixture

This independently authored Java 8 fixture creates an anonymous generic object
inside a lambda. The verification runner compiles its bodies with javac and
changes only the anonymous class's `EnclosingMethod` attribute to the exact
compiler-generated lambda implementation, which has no generic Signature.
It records an unchanged member-table hash. D8 performs the lambda desugaring.
No third-party application code or bytecode is used.

The JVM harness compares original and recovered captured reference identity,
forwarding, effects, callback exceptions and null behavior. It separately checks
the reported generic-reflection difference. Android SDK `dexdump` supplies the
complete input declaration inventory, including D8's added adapter, and javap
checks that recompilation retains every descriptor after the reported local
class rename. The recovered source requires Java 25 for pre-super capture writes.
