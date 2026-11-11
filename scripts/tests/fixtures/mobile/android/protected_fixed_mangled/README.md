# Protected fixed mangled method

A public nongeneric abstract owner extends a public plain parent. Its protected
final method accepts Press, long, and Sink<? super Value>, returns Object, and has
no method type parameters. A subclass in a different package invokes it on this;
a peer in the owner's package invokes it through the declared owner type.

The authored Java 8 method chooseValue is changed to the equal-length JVM name
choose-Name by editing only exact UTF8 constant-pool entries in the method and
caller classes. Code bytes and all other attributes stay unchanged. The runner
witnesses that change and accounts for the recovery's single identifier projection.
The frozen observer receives the declared name as data for reflection; direct,
compiled-caller and method-handle entrypoints retain their original legal names.

Observers cover actual result/reference identity, null inputs, wide values,
contravariant callbacks, ordered effects, output changes during callbacks, exact
exception identity, and original parent construction. Independent wrong programs
return the scope, narrow long arguments, skip the scope callback, or delay result
publication. An optional dependency contains the original Base class. No private
application data is used.
