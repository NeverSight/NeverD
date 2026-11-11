# Generic defaults overridden by an inherited class bridge

`Contract<K,V>` owns a generic default listener. `Base<K,V>` overrides it with a
covariant `Base<K,V>` result and the original javac forwarding bridge. `Middle`
and `Leaf` pass the full arguments through without redeclaring the listener.
An owned factory creates an anonymous `Leaf<String,byte[]>` with a protected
callback override that captures mutable owner state.

The immutable JVM harness checks base, middle, leaf, interface and reflective
listener calls, callback identity, original bridge casts, nulls and ordered
failures. A failing default body must never execute on the class hierarchy;
a separate default-only control confirms that body remains executable.
Source and unchanged declaration-JAR forms of the generic default are tested.

All original generic contracts and ordinary metadata are checked. The existing
anonymous-to-named-local source projection discloses changed binary/simple
names, reflection and compiler capture metadata; the runner maps those exact
changes and permits only reported helpers. Java 25 is required for the captured
constructor ordering. Metadata-only argument swaps and a one-byte original
bridge argument mutation must fail, with a separate javac scope counterexample.
