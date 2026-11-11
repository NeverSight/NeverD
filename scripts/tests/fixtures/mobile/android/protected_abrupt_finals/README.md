# Protected preparation with independent abrupt tails

A constructor has one normal return after its final-field assignment, a typed
catch that wraps a checked producer failure, and a separate guard rejection.
Both physical tails end in throw and have no protected instructions or final
stores. Every normal return must still cross the original assignment exactly
once; no handler may enter a tail interior or re-enter an assigned stage.

Source and SDK profiles compile all generated Java 8 units and compare every
original definition, metadata, Exceptions and helper role. A frozen future
subclass and observer run 64 fresh JVM cases twice. They check final-field
visibility on escaped partial objects, actual values, callback effects, exact
RuntimeException/Error identities and the original cause of checked failures.
Eight independently compiled wrong programs must fail the same observer.
Only authored fixture code executes.
