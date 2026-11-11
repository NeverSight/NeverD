# Original integer static receiver implementations

A plain interface fixes Comparable to its own type. The concrete typed compareTo
entry calls an original static helper with its unchanged receiver and argument;
only that entry gains ACC_BRIDGE in the owned input encoding. Its erased compiler
bridge and the interface's erased bridge keep their original bytecode and flags.

The immutable JVM harness compares 80 values, 144 identities, 237 states and 37
failures across eight routes. It checks full integer results including overflow,
repeated alias arguments, virtual callback effects, null/wrong inputs and exact
exception identity after each callback. Inventory, class/method/field scopes and
exception metadata stay unchanged except the disclosed typed bridge flag.

`--dependency-helper` retains the original Dispatch and Trace classes in an
unchanged JAR while the complete interface contract stays in input. Same-Code
synthetic flag drift and separately compiled reversed arguments/extra effects
must fail. The report accounts for the original typed entry and both checked
compiler bridges; coverage remains partial.

`--reference-result` additionally authors a covariant reference-returning entry
with one long argument. Its independent immutable harness compares 152 values,
296 identities, 456 states and 16 failures, proving both input words, reference
identity (including null), callback state and failure ordering. It retains six
classes/twenty-two methods; `--dependency-parent` keeps the original plain parent
in a JAR and retains five classes/twenty-one methods. Source helpers remain input.
A changed wide argument and synthetic typed flag must also fail. Both typed flags
and all four original compiler bridges are accounted for in the recovery report.

The `--inherited-predicates` profile adds two boolean entries declared by a
plain ancestor two edges above the closed receiver interface. Its separate
immutable oracle compares 80 values, 96 identities, 288 states and 32 failures,
including inherited reflection lookup, original helper callbacks, exact failure
identity and null receivers. Source and `--dependency-parent` modes retain seven
classes/twenty methods and five classes/eighteen methods. Three typed flags and
two original compiler bridges are reported; all other metadata stays unchanged.
Synthetic predicate flags and independently authored wrong receivers must fail.

`--omit-interface-bridge` removes only the owned fixture interface's original
compiler bridge before DEX creation, keeping all other Code and metadata intact.
The original five classes/twelve methods (or three classes/nine methods with
`--dependency-helper`) remain in the recovery inventory. One additional compiler
interface bridge is reported separately, with three projected original methods.
The ordinary comparison oracle still matches; an additional immutable oracle
checks 10 values, 22 identities, 34 states and 4 failures through inherited
reflection and MethodHandles. It measures the disclosed added default and
bridge declaration. Wrong complete interface arguments and a missing concrete
class bridge are rejected; coverage remains `partial`.

`--copied-receiver` authors explicit receiver copies and casts before the original
helpers. The integer, covariant reference and inherited predicate profiles use
their existing independent oracles and unchanged inventory/metadata checks.
The combined reference/predicate profile keeps eight classes/twenty-nine methods,
or five classes/twenty-six methods with unchanged parent declarations in a JAR,
and reports eight projected methods. The integer-only helper-JAR mode remains
three classes/ten methods. Complete cast receiver provenance, wide arguments,
original callbacks and ordered exceptions must agree; changed receivers and
extra effects remain rejected.
