# Scoped inherited interface default calls

An independent abstract `Child<E>` extends a generic class whose `subList`
returns `List<E>`, and implements `Facade<E>` with two parent interfaces. Its
original narrow override explicitly selects the inherited `Root<E>` default
through `Facade.super.subList`. The original class and interface List-return
bridges remain part of the definition and metadata inventory.

Frozen subclasses, callers, reflection and method handles observe the exact
interface implementation, original receiver and integer arguments, null and
polluted results, the existing result cast, result snapshots, partial effects,
initialization and RuntimeException/Error at each stage. Spy receivers/results
reject inserted queries. Same-Code signature mutations and independent wrong
programs protect the proof boundary. Source and SDK declarations share frozen
original class and observer bytes; all recovered Java is compiled and executed
only for this owned fixture.
