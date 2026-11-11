# Fixed Exceptions and generic class covariance

Owned javac inputs keep a generic Parent<T> returning Parent<T>, a plain far
superclass returning Object and a concrete Core returning itself. The zero-arg
method reads authored fields so casts, mutable aliases, exceptions and ordered
effects remain observable. First and Second are distinct checked exceptions;
all original typed and bridge declarations retain that ordered Exceptions list.

Source and immutable far-superclass-JAR profiles retain six classes/twelve
methods and five classes/ten methods, apart from reported private helpers. Three
original compiler bridges are regenerated with partial recovery receipts.
Complete metadata, original generic parent results and all runtime entries are
verified after recompilation; no generic metadata projection is added.

The immutable FixedExceptionCovarianceHarness compares 54 values,
1,728 identities, 180 state observations and 126 ordered failures through nine
direct, near/far superclass and typed/erased/reflected routes. An independent
subclass verifies virtual dispatch. Mutable field aliases, original exception
identities, null receivers and casts after partial effects remain observable.

Same-Code generic parent result and non-Throwable exception ancestor mutations
fail NeverD and independent javac. Original input, harness, far-superclass-JAR and
SDK hashes remain unchanged.
