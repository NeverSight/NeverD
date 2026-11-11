# Boxed superclass results under dependent bounds

This independently authored fixture composes the existing array target/factory
with `Bound<A,B extends Parent<A>>` and `BoundUser extends Bound<Box,Core>`.
The runner first compiles the original Object-return implementation, encodes its
verified original superclass argument and bridge flags, then separately compiles
the bounded consumer and immutable JVM harness against those frozen class files.
It does not derive original inputs or expected behavior from NeverD output.

Both source and dependency profiles retain the complete original parent argument
and dependent bound. One reported typed result entry contains the checked
original target/factory calls; javac supplies the original erased bridge.
Declared-method reflection and the extra forwarding frame are disclosed. All
original class, field, method Signature and Exceptions metadata remains checked.

Seven independently compiled invocation routes cover direct, parent, interface,
reflection, method handles and a generic bounded consumer. They compare nullable
and mutable array aliases, fresh/cached/null factory results, call counts, order,
and original exception identity. The oracle has 201 value checks, 969 identity
checks, 199 state checks and 133 expected failures in each profile. Same-Code
changes to the original parent argument, dependent bound and bridge flags reject;
an independent javac counterexample rejects an incompatible boxed override.

The source profile contains nine classes and eighteen original methods. The
unchanged dependency profile owns Api, Base, Parent and Bound, leaving five
classes and eight original methods for recovery. The new typed entry is counted
separately from the original inventory and ordinary emitter helpers.
