# Original scoped raw class parameter bridges

`Parent<P extends View>` declares an effectful generic read method;
`Middle<M extends View>` keeps its complete parent edge; and
`Core<T extends Button>` declares a Signature-free `read(Object, Button, Object)`
override with its original `read(Object, View, Object)` checked bridge. The
three-operation target calls the original superclass slot with unchanged
incoming arguments and scalar result.

The immutable harness compares 108 values, 558 identities, 360 states and
72 failures through nine direct, typed/raw ancestor and reflected routes over
two receiver implementations. An independent leaf override, mutable aliases,
bridge casts before effects, null receivers and target exception identities
remain matched. Source and immutable Parent/Middle-JAR profiles retain six
classes/nine methods and four classes/six methods, complete original metadata
and every original runtime entry apart from reported private helpers.
Coverage is partial only for one original compiler bridge; there is no generic
metadata or class-source projection.

Original D8 bridge and superclass-adapter opcode witnesses are required. A
same-Code class-bound change rejects in NeverD and independent javac. Native
controls reject missing or changed bridges, altered complete scopes, unknown
bounds, static/final/private drift and unproved superclass argument/effect
changes. Original input, harness, SDK and ancestor-JAR bytes remain immutable.
