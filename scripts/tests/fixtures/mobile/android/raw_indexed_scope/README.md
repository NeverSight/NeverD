# Raw indexed method scope

Independently authored Kotlin generates an indexed default callback with a raw
Function2 entry, Number.intValue conversion and a typed (int, K) Void overload.
Frozen callers cover standard and custom Number implementations, null/cast
precedence, one conversion, both descriptors, partial effects and exact failures.
An access-only compilation view exposes the default method to javac without
changing Code; that view is never executed or recovered. No application data is
included.
