# Parameter and return specialization through original covariance

Owned javac inputs keep Root<K, V> and Api<K, V> declarations and an authored
Core override of Root<String, Object>. Its key specializes to String and its
result narrows to Core. The one original Object/Object bridge casts the key,
invokes the typed String/Object-to-Core target and returns that same result.
No String/Object-to-Object intermediate entry exists. The target retains First
and Second unchecked Exceptions in order; the interface and bridge keep their
empty exception declarations.

Source and immutable Root/Api-JAR profiles retain five classes/eight methods
and three classes/five methods, apart from reported private helpers, and
regenerate only the original bridge with partial recovery. All original class
and method metadata, complete edge arguments and runtime entries are compared
after recompilation; no generic metadata projection is added.

The immutable ParameterReturnCovarianceHarness compares 108 values,
1,224 identities, 198 state observations and 90 ordered failures through nine
direct, typed/raw parent/interface and reflected routes. An independent subclass
checks virtual dispatch. Mutable aliases, nulls, original exception identities
and key casts before effects are observable. Reflection of the typed entry
retains its own wrong-argument failure rather than entering the erased bridge.

Same-Code class argument and unchecked ancestor mutations fail NeverD and
independent javac against the authored original override. Original input,
harness, Root/Api-JAR and SDK hashes remain unchanged.
