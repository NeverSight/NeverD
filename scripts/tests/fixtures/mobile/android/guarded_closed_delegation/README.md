# Guarded calls to closed constructor contracts

Unmodified Java25 bytecode has a raw descriptor-only constructor whose mask
branches select two distinct closed lists, a long and a float before this
delegation. Its target retains two nominal wildcard list signatures, and the
class retains a complete two-argument interface header. A no-argument entry
exercises the original constructor chain.
The same parent and interface sources target Java8 for dependency declaration
loading; their original classfile bytes remain unchanged in the SDK provider.

Source and SDK-parent/interface profiles compile all emitted Java and compare
every original definition, declaration metadata and Exceptions without adjusting
signatures. Frozen callers run 180 fresh JVM cases twice across reflection, two
MethodHandle routes and direct Java. Every mask, null/shared/unchecked list
identity, subclasses, long/float bits, partial fields and exact runtime/error
timing is observed. Sixteen independent wrong programs must fail before recovery.
