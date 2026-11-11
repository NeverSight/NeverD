# Typed handlers outside a synchronized region

Owned Java sources release a monitor before an ordinary callback with ordered typed exception handlers. The first matching handler may recover, another may rethrow, and a handler callback may fail independently. Original classes are compiled normally and converted by release D8 without patching.

A frozen observer compares reflection, method handles and a compiled caller. It checks exact exception and result identities, handler scope and order, callback arguments, null failures, repeated calls and monitor ownership. A competing daemon thread verifies exclusion during the locked callback and acquisition after every normal or abrupt completion. Wrong programs must fail before source recovery. Source and dependency-provider profiles preserve the complete method inventory, compiler metadata and Exceptions attributes.
