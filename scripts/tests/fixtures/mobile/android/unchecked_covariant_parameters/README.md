# Unchecked covariant parameter contracts

Owned javac inputs retain a plain interface returning itself and a concrete
implementation returning its own source class. Three original parameters include
an invariant Carrier<Number, String>. The typed target declares First and Second
in order; First extends RuntimeException and Second extends Error. The original
javac bridge retains the interface's empty exception declaration.

Source and immutable interface-JAR profiles keep five classes/seven methods and
four classes/six methods, apart from reported private helpers. All original class
and method metadata and runtime entries are verified after recompilation. Only
the original covariant bridge is regenerated, reported as partial recovery; no
generic metadata projection is added.

The immutable UncheckedCovariantParametersHarness compares 30 values,
960 identities, 100 state observations and 70 ordered failures through direct,
interface and typed/erased/reflected calls. An independent subclass checks virtual
dispatch. Mutable carrier aliases, original exception identities, null receivers
and casts after ordered effects remain observable. A bad carrier can throw before
the method reaches its declared failure, retaining partial effects.

Same-Code invariant argument and unchecked ancestor mutations fail NeverD and
independent javac. Original input, harness, interface-JAR and SDK hashes remain
unchanged.
