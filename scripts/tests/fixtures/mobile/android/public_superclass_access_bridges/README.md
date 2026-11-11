# Original public superclass access bridges

A public `Base` owns observable field and mutable-alias effects. Nonpublic
`Root<P>` overrides six public methods, adding effects before their Base calls,
and retains an abstract class-owned getter. Public `Core<G> extends Root<G>`
keeps its own getter and six original javac visibility bridges, each calling
its nearest Root method. An independent external harness leaf overrides all
six public slots, exercising dispatch through both child and public ancestor.

The immutable harness compares 368 values, 548 identities, 1928 states and
120 failures for void, boolean, integer, long, double and reference results.
Mutable aliases, raw NaN/zero bits, ancestor exception identity, null receivers,
reflection and complete getter scopes remain matched. Source and immutable
Base-JAR profiles retain three classes/24 methods and two classes/16 methods,
full class/method metadata and all runtime entries apart from reported helpers
and exactly the disclosed bridge/synthetic flag differences. Coverage is
partial for six original bridges, without generic or class-source projections.

Original D8 access-bridge opcode witnesses are checked. A same-Code final Root
method mutation fails NeverD and independent javac for the original explicit
super-delegation declaration. Native controls reject changed dispatch, receiver,
argument order, result origin, extra effects, full contracts, unknown ancestors
and static/private/abstract/final targets. Input, harness, SDK and ancestor JAR
bytes remain immutable throughout verification.
