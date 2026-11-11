# Recursive lower bounds over checked inlined interfaces

This extension keeps the authored `Api<Core>` original interface contract and
Object-parameter bridge encoding from `checked_inlined_interfaces`. It adds a
`Holder<T extends Api<? super T>>` with class-owned member types and a consumer
whose field and results use the complete `Holder<Core>` argument.

The recovered bridge remains in its original runtime slot and source declares
`Api<Object>`. That argument satisfies the unchanged lower bound. A raw source
interface fails the same independent javac check; invariant and upper bounds
are separately rejected by the native source-bound recheck. No bound is erased
or silently bypassed.

The immutable harness compares 286 values, 1,899 identities, 1,045 states and 66
failures across eleven routes, including typed/raw holder dispatch, consumer
return casts, aliases, mutable markers and argument-before-receiver exception
order. All Holder and consumer metadata and original runtime entries remain,
apart from reported private helpers. Source and unchanged interface-JAR
profiles both reject same-Code interface-argument and synthetic-flag drift.
