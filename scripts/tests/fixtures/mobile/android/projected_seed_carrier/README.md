# Projected static wrapper and independent method seed

These sources are independently authored. The runner compiles a generic wrapper,
a callback and a side-effecting static method with its own `V` formal, then changes
only one classfile Signature argument from `Object` to the wrapper's `T` and marks
its exact constructor, box and unbox methods synthetic. This encodes a valid JVM
scope that Java source cannot spell in a static method; it is not Kotlin compiler
output. Each edit checks that all method Code and descriptors remain unchanged.

The frozen caller and observer execute the authored input, encoded input and
recompiled NeverD output through direct calls, reflection and method handles.
They compare argument/result identity, mutable callback state, exception identity,
nulls and ordered effects. Reflection checks the disclosed `T` to `Object`
projection and requires the independent method formal `V` to survive. All other
metadata, original class/method inventory and Exceptions are reconciled. Three
independent wrong-code variants must fail the observer. The dependency variant
freezes only the callback interface, keeping wrapper bodies available for proof.
