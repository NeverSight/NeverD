# Final array data payload

Owned javac/D8 input contains one public final int array, an array data payload
and a NOP aligning its nonexecuted payload after the initializer return. The
frozen observer checks all original cells, final/static reflection metadata,
array identity, mutable aliases and direct/reflected/method-handle dispatch:
30 calls and 12 expected original failures. All original class/member metadata,
Exceptions and runtime entries are compared after source recompilation.

An independently compiled changed array cell must fail that observer. Original
classfiles, observer and SDK bytes remain unchanged. Native tests cover all eight
primitive array types, fills before/after publication, optional trailing NOPs,
genuine edges into padding, repeated writes, malformed operands and payload widths.
