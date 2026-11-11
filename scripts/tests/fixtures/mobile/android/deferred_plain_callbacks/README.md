# Deferred plain interface callbacks

The abstract generic parent implements both `Order<T>` and a nongeneric
`Updates` interface. Three `Updates` callbacks have no parent declaration;
the concrete child supplies them. The parent's existing payload callback
records effects before invoking the child's original two-argument callback.

An immutable JVM harness compares child, parent and interface dispatch,
primitive edge values, identity-bearing references, exact state and effect
order, target exceptions and null receivers. Reflection and the complete
classfile inventory reject invented parent methods or altered generic scopes.
The runner exercises source and declaration-JAR forms of `Updates`, and rejects
a missing child provider and competing generic scopes without changing Code.
