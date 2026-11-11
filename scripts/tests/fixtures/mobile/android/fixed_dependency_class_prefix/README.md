# Fixed dependency class prefixes

The owned dependency keeps a plain concrete `Widget` above an abstract `Fixed`
class whose original superclass Signature fixes `Base<T>` to `Base<String>`.
The deeper plain root, generic SDK methods and two original SDK bridges stay
in the unchanged JAR. The original raw-parameter fixture's forwarder extends
this prefix and retains a protected override without class-owned formals.

Run `test_mobile_android_original_raw_parameters.py --dependency-interface
--dependency-parent-chain`. Its immutable wrapper checks complete inheritance
and formal reflection, constructor state, mutable aliases, inherited method
ownership, direct and reflected protected dispatch, virtual SDK overrides,
and effects before the exact original failure object. A superclass arity
mutation preserves Code and descriptors but fails NeverD and independent javac.
All seven input classes and fourteen methods retain their complete original
metadata. The wrapper adds seventeen identity checks, seven state observations
and three ordered failures, with zero source bridges.
Unknown or raw generic edges, captured scopes, source transitions, invalid
overrides and input overrides of class-owned SDK formals require other proofs.
