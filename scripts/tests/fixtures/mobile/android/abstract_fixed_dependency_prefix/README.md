# Abstract prefixes above fixed dependency providers

This profile extends the fixed dependency class-prefix fixture. Widget is
abstract and introduces no abstract method; ConcreteWidget keeps the original
item/set implementations and two SDK bridges above Fixed and Base<String>.
Five original SDK classes remain in the frozen JAR. Input methods, class
metadata, generic member ownership and runtime entries remain unchanged.

Run test_mobile_android_original_raw_parameters.py with --dependency-interface,
--dependency-parent-chain and --abstract-fixed-prefix. The immutable harness
compares the base 36 values, 444 identities, 156 states and 20 failures, plus
17 SDK identities, seven states and three ordered failures. It checks the
abstract/concrete roles, inherited provider ownership, reflection, mutable
aliases and virtual protected dispatch; no source bridge is added.

Changing the SDK to introduce an abstract method or reabstract an existing
concrete member leaves the original input DEX untouched. Both NeverD and an
independent javac compilation must reject the missing implementation. The
original superclass-arity and raw-parameter counterexamples remain active.
