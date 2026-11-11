# Specialized dependent method bounds

An interface declares method variables bounded by its own class variable.
Two original implementations specialize that class argument to a final nominal
value class: an ordinary class and a named member class with a captured outer
instance. Method variables retain their distinct scopes and their dependent
chain while their source erasures follow the concrete upper bound.

The original javac bridges cover a void consumer, an array-producing method with
a dependent second parameter and contravariant callback, and a plain value
method. Independently compiled callers, reflection and method handles compare
936 identities, 700 states and 98 ordered failures. Callback writes, array
aliases, nulls, wrong erased arguments, exception identity, captured outer
updates, class/field/method metadata and original bridge flags are checked.

Source and immutable dependency profiles also support the original single
interface-slot bound encoding. That input encoding changes only the disclosed
source bound-position spelling. Same-Code negative signatures change a bound
chain, callback formal or variance; independent javac also rejects the unrelated
method bound. Native fixtures include array parameters, ordering, direct and
linked entry points and corrupt original bridge origins.

The captured owner uses the existing explicit-outer static member projection.
Its constructor ABI and field remain unchanged; only the disclosed inner static
modifier and synthetic capture-field flag differ. Source recovery retains six
classes and twenty methods; immutable declarations leave three input classes
and fifteen methods. The six original bridge projections and four captured
member projections are accounted for separately.
