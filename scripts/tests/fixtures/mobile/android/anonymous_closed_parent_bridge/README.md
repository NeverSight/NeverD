# Anonymous closed parent bridge fixture

This owned anonymous class extends a plain class whose original Signature closes
an interface's formal to Value. Its compiler bridge checks two Object parameters
and forwards a float context to its own typed method, retaining an Object result
slot. The method calls its superclass and observes the captured Host instance.

Source and SDK profiles compile every emitted unit and compare original Code,
access, bridge flags, Signatures, Exceptions, class identities and exact helpers.
The existing anonymous-to-named-local receipt discloses one binary-name mapping,
one additional enclosing constructor argument and the explicit capture field's
synthetic flag; every other compiler fact must match.

Five frozen caller routes cover typed, raw interface, generic, reflection and
method handle dispatch. Each profile runs 160 fresh JVM cases twice, including a
future override, both failing cast positions, nulls, partial writes, exact
checked/runtime/Error objects, aliased inputs, opaque results, NaN payloads and
signed zero. Sixteen wrong programs qualify the observer before decompilation.
Eight same-Code Signature controls retain complete ancestor scope checks. No
proprietary input is included.
