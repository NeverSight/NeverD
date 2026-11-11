# Constructor argument loops

First-party Java 25 compiler input with two loops before an exact `this` call.
The independent frozen caller, reflection and method-handle observers check
zero and repeated iterations, null and identity values, full long words,
ordered failures, and partially initialized final fields. The runner recompiles
all recovered sources, matches every original entry and its class metadata,
and rejects four independently compiled incorrect programs. An additional
profile imports the original Trace class as a dependency declaration.

The native tests exercise both invoke encodings and declaration orders and
reject receiver escapes, mixed receiver joins, and entry into move-result.
