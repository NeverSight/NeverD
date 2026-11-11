# Original result upcast bridge

This independently authored fixture specializes `Parent<T extends Base>` with
`Leaf`. Its compiler-generated `create() -> Base` bridge receives one explicit
`CHECKCAST Base` before returning the unchanged typed result. The runner edits
only this owned Code attribute and records the mutation; descriptors, access
flags, signatures and every other method remain unchanged. The ordinary
`checked(Object)` method supplies the existing Base class constant.

The JVM baseline runs the edited original bridge. Recovered sources must retain
the complete generic parent contract, original method inventory and regenerated
bridge flags, with only reported private source helpers allowed. The harness
compares object and null identity, virtual overrides, target effects and exact
exceptions. A same-size cast change to Receiver must fail recovery rather than
being discarded as an upcast. No third-party code is executed.
