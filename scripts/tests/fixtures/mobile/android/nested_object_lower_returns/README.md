# Closed nested Object lower-bound returns

The authored final `View` overrides a generic interface default returning
`Iterator<Pair<? super Object,? super Object>>`. Class-file encoding records
its original return as `Iterator<Pair<Object,Object>>`, preserving every Code
byte, descriptor and other Signature. Both spellings describe the same closed
nested type: Object is the only possible argument of its Object lower bound.

The common signature validator records `closed-object-lower-bound` after
comparing the complete original nested result and inherited declaration. The
inheritance consumer uses that exact witness. Source keeps the original method
Signature, runtime entry and body; no bridge or generic metadata projection is
introduced. Abstract and default, source and dependency contracts share this
proof. Other lower bounds and open scopes remain outside it.

Nine direct, typed/raw, original-caller, reflected, MethodHandle and default
routes compare 18 values, 153 identities, 117 states and 27 failures. Returned
mutable iterators preserve shared identity, removal, alias updates, exhaustion,
nulls, repeated effects and exact exceptions. Complete class, field and method
metadata remains unchanged, apart from separately reported private helpers.

An independent javac check accepts the original closed return with unchecked
warnings treated as errors and retains its exact Signature. Different complete
arguments fail both NeverD and javac. Same-Code return/ancestor-bound mutations
and native scope, role and exception controls remain fail-loud.
