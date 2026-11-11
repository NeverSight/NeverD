# Fixed input callback and completion binding

A compiler-produced static generic method accepts a callback with a closed Long
input and one Object-bounded result, plus a completion consuming that same
result. Frozen callers and implementations audit raw dispatch, saved callbacks,
completion identity, arbitrary suspension sentinels, wide input values, repeated
calls and ordered provider/callback/completion failures. All original class and
method metadata is compared before and after recovery; no metadata edits.

`--instance-provider` authors the same complete method relation on a public
instance implementation and an opaque generic service interface before javac.
Source and dependency-interface profiles retain the original interface call,
actual receiver and alternate implementation. Four frozen call routes cover
64 fresh JVM cases with two attempts, including null service receivers,
deferred callbacks, actual suspension sentinels and exact partial failures.
Six wrong Java programs must be detected, including replacement of the service
receiver and loss of its captured identity. Every recovered Java compiles and
all original entries, generic metadata and Exceptions remain unchanged.
