# Nested anonymous constructor arguments

A recursive generic owner creates one anonymous interface callback in its
static initializer, as a later argument to a concrete constructor. Original
source and SDK layouts exercise class initialization before argument effects,
unique anonymous allocation, constructor stores and callback invocation.
The conditional variant uses a switch expression to retain the callback in a
local reference and select that reference or null using a mutable input before
the outer constructor. Both paths execute. DEX inspection independently confirms
the branch and constructor ordering.

The frozen observer checks direct, reflective and method-handle entry paths,
plain, future and null contexts, changing argument fields and boolean results,
partial construction, failed initialization and repeated access. A saved
anonymous object remains callable after its enclosing initializer fails.
Checked, runtime and Error objects retain their original identity and JVM
initialization wrapping. Independent wrong programs qualify these observations.

All emitted Java compiles. Every original class and method is compared through
independent classfile facts, including anonymous binary identity and enclosing
metadata, Code roles, access, Signature, Exceptions and parameters. Only the
existing exact throw helper contract is admitted as an extra runtime entry.
The source report keeps its anonymous projection explicit even when the owned
compiler comparison independently confirms the original identities.
