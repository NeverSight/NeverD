# Inherited consumers with original scalar carriers

The int and long profiles implement a closed inherited consumer through a final
Carrier and an originally named writeValue target. The existing erased write
body checks Carrier, calls its readBits method, and invokes that exact target.
Recovery must retain the original call effects and names, without treating a
carrier as a standard Number or dropping producer/metadata/marker contracts.

Input encoding changes only the authored raw interface Signature and original
consumer bridge flags; Code attributes are unchanged. Recompilation compares the
complete runtime inventory and all metadata outside the reported raw edge and
explicit bridge flags. Source and immutable dependency profiles retain exactly
the original Carrier methods, including arbitrary unbox side effects.

Immutable callers exercise six invocation routes, both scalar widths, boundary
values, nulls, malformed erased inputs, original unbox failures and target
failures. The unbox call must occur before target effects and preserve both words
of long arguments. Producers keep their original result identity and other
interface methods retain their full contracts. Native controls reject open or
nonfinal carriers, static/generic unbox methods, incorrect casts and words,
missing delegates, synthetic flags and incompatible sibling bindings.

Each source profile has seven classes and fourteen methods; immutable interfaces
and Carrier leave two source classes and seven methods. Each JVM matrix checks
52 values, 90 identities, 67 states and 36 ordered failures.
