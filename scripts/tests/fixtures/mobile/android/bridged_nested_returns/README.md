# Bridged nested return variance

Core<K,V> inherits Base<Pair<? extends K,? extends V>, Token<K,V>> through
Middle<K,V>. Its protected values(Token<K,V>) returns an iterator of invariant
Pair<K,V> in the encoded input, while the parent requires nested wildcards.
The unchanged original public erased bridge checks Token and calls that target.

The independent javac baseline is compiled with the valid wildcard return;
encoding changes only the target Signature and original bridge access. Every
Code byte, runtime descriptor and immutable caller stays unchanged. Recovery
must report the exact original and source Signature, inherited witness and typed
access promotion. Full class/method/field metadata and Exceptions attributes
are compared, including compiler bridge flags and the complete runtime inventory.

Eight invocation routes check returned iterator identities, mutable contents,
null inputs and results, cast failures, target effects and exception ordering.
Source and dependency parent modes both retain all original dispatch slots.
Native and encoded counterexamples reject mismatched argument/return scopes,
lower-bound witnesses, wrong casts or incoming registers, missing bridges and
missing synthetic flags. Independent javac rejects the invariant nested return.

Each JVM matrix checks 46 values, 136 identities, 100 states and 60 failures.
