# Plain interface fixed Exceptions and complete method formals

The owned nongeneric `Api` has a plain read member with two fixed checked
exceptions and `<T> T unwrap(Class<T>)` with one fixed exception. `Core<G>` keeps
its own unused class formal and complete method-owned scope. Both source and
immutable root-JAR profiles retain original exception declaration order.

The immutable `PlainFixedExceptionsHarness.java` checks direct/interface/
reflected dispatch with an independent override, mutable aliases, nulls,
ordered target effects, original checked exceptions and Class casts after
those effects. Each profile checks 48 values, 536 identities, 104 states and
56 ordered failures. Full metadata and inventory recompile unchanged apart
from reported private helpers, with zero bridges or metadata projections.
The source profile has four classes/seven methods; the dependency profile
three classes/five methods. Coverage is `recovered`.

Changing only an original method's formal bound while retaining Code and runtime
descriptors fails NeverD and independent javac. Native controls require the shared
closed exception receipts, full method bounds, parameter/result scopes and
access roles. Original input, harness, dependency JAR and SDK hashes stay intact.
