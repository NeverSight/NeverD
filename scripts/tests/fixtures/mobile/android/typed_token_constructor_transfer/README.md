# Typed token transfer before constructor delegation

Two public constructors retain one original bounded class formal. The input
constructor performs opaque checks and a static invariant unary token transfer
before delegating to the output constructor. The mapper's method formal stays
distinct from the class formal; its returned object may come from mutable state.
Frozen observers check exact effects, failures, partial final fields, context
mutation, actual output identity, deferred callbacks and late casts.

The authored prefix uses Java 25 syntax. The runner retains compiled classes,
then changes only their class-file major header for the older D8 reader,
verifies all other bytes stay identical, and executes that exact input on
Java 27. Recovered sources compile at the report's selected release; both
version transitions are recorded separately from declaration metadata.

The `--allocated-transfer` profile constructs an independent unary
`Transfer<T>` from the original `Input<T>` and obtains the actual `Output<T>`
through its original instance method before `this(...)`. The class formal,
constructor input and result must retain one complete invariant relation.
Signature-only controls keep all original Code bytes and reject unproved
variance, raw results, unknown formals, bounds and throws. Both the retained
Java 25 classes and the header-only D8 inputs run against the frozen observer.
