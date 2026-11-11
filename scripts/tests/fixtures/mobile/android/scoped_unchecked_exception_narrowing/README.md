# Scoped unchecked exception narrowing

Owned javac inputs retain a generic parent interface with a fixed checked
exception, an abstract generic subinterface with independent unchecked Exceptions,
and a concrete generic implementation. Both class-owned read parameters/results
and alpha-equivalent method-owned Class/result scopes remain original. First is a
RuntimeException; Second is an Error. Source and immutable root-JAR profiles
retain six classes/ten methods and five classes/eight methods, with no bridges or
metadata projections apart from explicitly reported private helpers.

The immutable ScopedUncheckedExceptionNarrowingHarness compares 72 values,
804 identities, 156 state observations and 84 ordered failures across direct,
parent/subinterface and reflected calls. An independent subclass exercises virtual
dispatch. Mutable aliases, null receivers, original exception identities and
Class casts after effects remain observable. All class/member metadata, ordered
Exceptions declarations and runtime inventory are checked after recompilation.

The runner changes a method-owned bound and changes First's unchecked ancestor
to Exception while retaining original Code bytes. NeverD and independent javac
both reject those complete-contract counterexamples. Input, harness, root-JAR and
SDK hashes remain unchanged.

The `--checked-exceptions` profile uses owned copies where First extends
RequiredFailure and Second extends First. The same frozen observer verifies
checked narrowing through both class-owned and method-owned generic contracts,
ordered Exceptions and both input/immutable-root profiles. Its same-Code
counterexample widens First to Exception, outside the required checked subtype.
