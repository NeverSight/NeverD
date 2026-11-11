# Original fixed exception subsets

The owned plain `Api` declares checked `First, Second` and an additional
unchecked `UnusedFailure` on its read member. Its method-owned generic unwrap
member declares `First, UnusedFailure`. `Core<G>` retains narrower nonempty
checked lists and the full alpha-equivalent method-owned scope.

The immutable `FixedExceptionSubsetsHarness.java` compares 48 values, 536
identities, 104 states and 56 failures through direct/interface/reflected and
independent subclass dispatch, mutable aliases, checked exception identities
and casts after effects. Source and immutable interface-JAR profiles retain
five classes/eight methods and four classes/six methods, all original metadata
and runtime entries apart from reported private helpers, with zero bridges or
metadata projections and recovered coverage.

Run the plain fixed exception runner with `--exception-subsets`. The original
method-bound counterexample and an Exceptions-only mutation removing the
required checked exception fail NeverD and independent javac, without changing
Code or Signatures. Native profiles also cover reordered and single-entry
subsets, abstract subinterfaces, source/SDK roots, scope drift and missing
closed receipts. The proof compares exact exception keys and does not infer
new checked exception subtype relations. Input, harness and SDK hashes stay intact.
