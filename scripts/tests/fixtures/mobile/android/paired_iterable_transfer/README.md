The owned paired iterable fixture checks two independent static entry contracts:
`<K,V>(Map<? extends K,? extends V>,Iterable<? extends Pair<? extends K,
? extends V>>)Map<K,V>` and the corresponding lower-map/void writer. Result
and --void-entry profiles each run source and frozen provider/pair modes.
Each mode runs165 fresh JVM cases, two attempts per case, through five routes.

Frozen map, iterator and getter callbacks replace external references and future
rows. Observers independently check iterator creation/hasNext/next timing,
wrong raw elements and late typed-map bridge checks, exact key/value identities,
partial puts, discarded arbitrary results, input/output snapshots and the actual
late returned map (including null and a distinct replacement). Iterator creation,
first and later predicates/next/getters, map calls and first/repeated static
initialization fail with their original RuntimeException/Error identity.
Full reflection retains both own Object formals, the complete unary iterable/
binary pair relation and upper-map/invariant-result or lower-map/void slots.

Every generated Java file compiles. SDK/compiler definitions, full metadata,
Signatures and Exceptions must match. Ten unchanged-Code Signature mutations
reject in every mode. Ten independent wrong programs fail both observers;
three additional result-profile programs expose returned-input inference and
early output/returned snapshots. All original classes, DEX, authored void
sources and immutable observers are hashed before recovery and checked again.
