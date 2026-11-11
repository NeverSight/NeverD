# Dependent method bounds

This owned fixture retains a class-dependent method bound and a second bound
referring to the first method formal. Alpha-renamed API and implementation
formals must agree through arrays and callback lower wildcards, while the
unrelated class formal and class-owned `value` member retain their own scopes.

The runner covers ordinary class-bound encoding and an alternate single
interface-slot encoding, with either source interfaces or an immutable
interface JAR. Only the original bound Signature bytes are changed for the
alternate encoding; the common resolver discloses that source spelling change.

An immutable harness and separately compiled caller check 352 identities, 176
state observations and 24 ordered failures through eight dispatch paths. They
check original callback ordering, array identity, nullable values, exception
identity and full generic reflection. All runtime descriptors, class/field and
method metadata, declared exceptions, input bytes, harness bytes and dependency
bytes are compared. Three source classes/six methods or one source class/three
methods are recovered without new method projections. Same-Code bound drift
fails recovery, and an independently authored invalid override fails javac.
The caller stays in the immutable oracle because general generic invocation
requires a separate proof from inherited declaration compatibility.
