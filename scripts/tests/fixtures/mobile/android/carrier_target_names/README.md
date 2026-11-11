# Local carrier target JVM names

This owned javac fixture has two checked virtual long conversions, an original
long/long target, and a static result factory. The runner encodes final anonymous
class and InnerClasses flags, the full interface Signature, bridge flags, and a
JVM-only target UTF8 name. Every original Code byte remains unchanged and that
exact encoded input executes before D8.

Each source/dependency profile checks 36 fresh JVM cases with two attempts and
four interface call routes. It also invokes the primitive target through
reflection and MethodHandle using the exact reported spelling. Wide edge values,
factory identity, ordered casts and nulls, original exception objects, complete
metadata and inventories are checked. Three independently compiled incorrect
bodies must fail the immutable observer. The class/name mappings, raw interface,
bridge flags and generated helpers are explicitly accounted for; original
binary calls by the old primitive name are not claimed to survive.

The `--enclosing` profile creates an additional anonymous class in the target.
Its encoded EnclosingMethod follows the original JVM name without changing any
Code bytes. Reflection checks the separately reported source EnclosingMethod,
receiver capture identity, the explicitly disclosed constructor prefix and
capture-field flags. External metadata references remain rejection cases.
