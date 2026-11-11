# Object-root covariance

The original Java compiler generates the Object-return bridges for generic
`Core<T>.clone()` and `Broken<T>.clone()`. Both typed entries retain their full
self type and checked `CloneNotSupportedException` attribute. `Core` implements
`Cloneable`; `Broken` deliberately does not. A recovered generic child inherits
the original bridge without acquiring another declaration.

The runner preserves the original DEX inventory, compiles every recovered class,
checks exact method/class/field/generic/Exceptions metadata and an independently
specified throw-helper inventory, then runs frozen direct, reflection,
method-handle, external-subclass and source-caller tests. Clone identity, shared
field references, mutable counters, failure identity and native clone failure
order are checked. The regular profile uses ordinary javac and D8 output.
The `--omit-original-exceptions` profile removes only the typed and bridge
Exceptions attributes from the owned classfiles, certifying unchanged Code,
descriptors and constant pools before D8. It checks that the typed entries stay
exception-free and that the inherited checked exception regenerated on the
bridge is the exact metadata difference disclosed in the recovery report.

Removing ACC_SYNTHETIC keeps original Code unchanged and must fail recovery.
Independent javac rejects widening the Object override's checked exception.
Native tests cover malformed platform roots, final/static parent slots,
missing or altered bridge bodies and receiver/exception drift, through direct
and linked input with reversed class and method order.

The `--narrowed-child` variant gives the original generic child its own narrower
clone result, with both the parent-return and Object-return compiler bridges.
The source and `--sdk-parent` runs compile every emitted source and retain exact
class, method, field, Signature, Exceptions and bridge metadata. A frozen future
subclass exercises all three result slots through direct calls, reflection and
method handles. Eighty child cases run twice in fresh JVMs, checking copied and
original fields, null results, virtual overrides and exact checked exception
objects before and after the parent call. Ten wrong programs qualify these
observers. Native controls also cover closed and open parent bindings, alpha
renaming, range frames, class/method order and malformed original forwarding.
