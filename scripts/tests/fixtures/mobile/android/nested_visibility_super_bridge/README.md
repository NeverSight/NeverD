# Nested superclass visibility bridges

Owned Java sources exercise a public static member extending its protected static
generic sibling. A package-private generic ancestry supplies scalar, reference
and void methods; the protected member also supplies a bounded-formal input.
The compiler creates the public visibility bridges. The test preserves their
original descriptors, flags and exact normalized forwarding bytecodes.

Frozen callers use direct calls, reflection and exact method handles, including
future overrides. Independent callbacks observe receiver and argument identities,
wide values, raw floating-point bits, returned identities and exact failures.
Raw ancestor, reflection and method-handle routes retain a broad original input
through the erased visibility bridge. A separately compiled future override
retains its own original narrowing check before entering the method body.
No third-party names, code or data are included.
