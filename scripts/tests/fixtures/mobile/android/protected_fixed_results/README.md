# Protected final results with fixed arguments

An abstract Core<E> implements Base<Slot> through a protected final get method
with Token, int, long and double parameters. Its original public erased bridge
forwards all incoming words unchanged and returns the original Slot as Object.
The final target invokes a protected virtual resolver; an independently compiled
subclass overrides that resolver, retaining callback dispatch after recovery.

Input encoding changes only the original compiler bridge visibility. Full
runtime inventories, class/method/field metadata and Exceptions attributes are
compared after recompilation. Only the typed target's protected-to-public access
change is allowed and reported; final modifiers and every runtime slot remain.

Source and immutable dependency parents exercise six routes, reference aliases,
null inputs/results, long extrema, double signed zero and NaN payload bits,
ordered target/callback effects and exception identities. Each JVM matrix checks
576 values, 864 identities, 960 states and 294 failures. Native controls reject
wrong fixed arguments, split wide pairs, result origins, scope/access drift,
invalid source overrides and incorrect result casts. Encoded and independent
javac controls reject incompatible inherited result bindings.
