# Empty original bridge exceptions with checked ancestors

The owned generic parent and interface declare two ordered checked exceptions.
A final child's original typed method calls that parent after updating state;
its erased synthetic bridge checks and forwards the complete argument list.
The runner removes only the original typed and bridge Exceptions attributes,
checking that every Code byte, descriptor and constant-pool entry is unchanged.

The immutable JVM harness verifies all original dispatch paths, checked failure
identity, mutable aliases, repeated effects, nulls, argument casts before entry
and result casts after the parent's effects. Rebuilt typed throws remain empty.
Only the regenerated erased bridge gains the ancestor's ordered Exceptions,
which must be disclosed in the recovery report and checked independently through
reflection. Source and unchanged ancestor-JAR profiles exercise the same oracle.
Same-Code wrong superclass arguments and missing synthetic flags remain rejected.
