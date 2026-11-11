# Inherited original implementations

A plain class inherits two original ACC_BRIDGE implementations through an
abstract source class chain: one method-owned generic declaration and one fixed
parameterized declaration. The owned class-file edit changes only flags, and
D8 must retain all six classes and eleven methods (five/nine with the original
interface in an immutable dependency JAR).

The immutable harness is compiled before those edits. Eight call paths across
ordinary and overriding receivers check 928 identities, 416 states and 96
ordered failures. Callbacks observe the original field writes, mutable values
retain their aliases, and exceptions retain their identity and order. Reflection
requires the original parent provider, complete method bounds and arguments;
only the reported bridge/synthetic flags and private helpers may differ.

Both flag profiles are tested with source and dependency interfaces. Same-Code
changes to an unused formal's bound, result formal or fixed generic arguments
must fail recovery; independent javac rejects the changed-bound override.
Neither the original provider nor its body is copied into the descendant.
