# Bounded covariant method results

These independently authored source interfaces are RootContract and Contract, whose
method-owned Bound formals feed ResultBase<M> and ResultMid<M> returns.
View uses an alpha-equivalent R and returns ResultLeaf<R>, whose original
generic ancestry proves both complete result substitutions. Javac supplies
the two original class bridges and the interface bridge. The ordinary case
changes no input Code or metadata. The optional omission case removes only the
owned compiler interface bridge, with a receipt and all other Code preserved.

The JVM oracle checks typed, interface and subclass dispatch, object/null
identity, original bridge flags, method bounds, generic scopes and ordered
failures. A metadata-only ResultLeaf ancestry change to ResultMid<OtherBound>
keeps all Code and runtime descriptors, but must fail the complete return
proof. NeverD and javac both reject that different generic result path. When
the original interface bridge is absent, the exact additional javac slot,
target and reflection/default/dispatch differences must be reported explicitly.
