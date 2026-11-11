# Same bound generic key lookup

An invariant Key<E> and a generic method's E result declare exactly the same
single interface bound. Frozen callers and implementations check actual lookup
identity, nulls, provider state, failed reads, malicious raw providers and the
original bound cast. Every original Signature, declaration and Exceptions
attribute remains intact. SDK variants borrow only independently compiled
interface declarations; no input metadata is edited.

The `--static-context` variant copies the sources into a fresh directory and
turns the original receiver into an explicit Provider parameter. All four frozen
call routes still check the same provider's ordered state, including two calls,
null context, skipped reads, key failures and returned identity. Reflection and
method handles resolve the original static descriptor. Both declaration modes
run 40 cases twice and compare every original method and class metadata entry.
