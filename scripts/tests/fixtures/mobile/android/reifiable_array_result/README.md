# Result-only arrays with closed contexts

This independently owned fixture keeps a public static Object-bounded method
formal solely in its array result. Opaque `Class<?>`, receiver and array contexts
supply no binding. A separate entry performs actual reflective array creation
from a Class token and length, retaining its original Object-array cast.

Frozen observers run 122 fresh JVM cases twice. They distinguish arbitrary and
null results, actual array allocation and component types, primitive-array and
caller casts, ignored calls, negative sizes, null/void tokens, array stores,
callback snapshots, late writes, fresh arrays and exact failure timing/identity.
Twenty wrong programs fail before recovery. Source and dependency profiles
compile all emitted Java8 units and preserve complete definitions, metadata,
roles and Exceptions. Eight Signature-only controls preserve original Code.
