# Generic source parents with scoped nongeneric ancestor members

The owned nongeneric `Base implements Api` retains a method-owned generic
unwrap member, ordinary read member, ordered fixed checked Exceptions and
original effects. `Root<P> extends Base` declares a class-owned getter;
`Core<G> extends Root<G>` retains its own scope and inherits Base's methods.

The immutable `ScopedPlainAncestorsHarness.java` compares 58 values, 546
identities, 104 states and 56 failures through direct/interface/reflected
inherited calls, typed/raw/reflected parent getters and an independent override.
Mutable aliases, Class casts after effects and checked exception objects stay
matched. Source and immutable Base/Api-JAR profiles keep six classes/eleven
methods and four classes/six methods, full metadata and all runtime entries
apart from reported private helpers, with recovered coverage and zero bridges
or metadata projections.

A same-Code ancestor method-bound mutation fails NeverD and independent javac.
Native controls require complete method-owned/parameterized contracts, original
static/final/private roles, constructor presence and known bounded ancestry.
Original input, harness, dependency and SDK hashes remain unchanged.
