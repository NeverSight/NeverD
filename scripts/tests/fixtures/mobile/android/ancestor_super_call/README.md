# Symbolic ancestor superclass calls

An owned generic class calls inherited `Object.clone` through an independent
generic parent and retains its original cast, field updates and exception
handler. The runner verifies D8 keeps the compiler's original ancestor reference.
All original compiler entries, metadata and observer inputs remain frozen.

A plain instance throws the original clone failure wrapped in InternalError.
A future caller-owned Cloneable subclass exercises native cloning of the actual
runtime class, copied final fields/back references, constructor counts, partial
updates, exact failures and delayed casts. Its overriding virtual clone must
remain bypassed by lexical super dispatch. Source and SDK-parent profiles
compile every recovered unit and retain every original entry and role.
