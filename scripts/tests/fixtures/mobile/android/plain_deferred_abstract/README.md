# Plain deferred abstract class declarations

This extends the abstract dependency-parent profile with two plain abstract
input classes and an original concrete subclass. `Deferred` inherits the
SDK getter and protected callback and reabstracts its method-owned identity.
`Redeclared` retains original abstract getter, callback and alpha-equivalent
identity declarations. Their class scopes have no formals or Signature.

Run `test_mobile_android_original_raw_parameters.py --dependency-interface
--abstract-dependency-parent --deferred-abstract-child`. The immutable JVM
wrapper verifies inherited versus redeclared owners, protected access,
method-formal names and scopes, typed and reflected dispatch, mutable aliases,
raw callback capture and state changes before the same exception object. Its
independent concrete subclass is compiled only against the original classes.

A changed abstract callback formal retains absent Code and the same descriptor
but fails NeverD recovery and independent javac. Every original class, field,
method and inventory entry recompile with complete metadata retained and zero
bridges. Ten input classes and twenty-nine methods retain their inventory.
The wrapper adds eighteen values, 128 identity checks, eighty state
observations and sixteen ordered failures to the existing raw and SDK oracles.
