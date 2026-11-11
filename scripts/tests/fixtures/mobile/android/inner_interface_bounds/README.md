# Inner member interface bound fixture

These independently authored Java sources use a real nonstatic member of
`Owner<T extends Item>`. The interface declares an executable member contract.
The compiler supplies the explicit outer capture and pre-super write. The runner
adds only `Owner<T>` Signature metadata to that synthetic capture field, matching
the standard generic enclosure; all original Code attributes, field descriptors
and runtime method descriptors must remain byte-for-byte unchanged.

The javac/D8/JVM oracle compares two interface implementations, mutable captures,
32 returned identities, 13 ordered failures, the explicit constructor arguments
and the member's independent shadowing method formal. It confirms the original
interface upper bound on the public owner and five captured metadata projections.
The parameterized member qualifier and nonstatic reflection changes are reported
separately. Java 25 recompiles the explicit outer write before super.
No private artifact is included.

The `--class-bound` profile compiles the same owned contract as an abstract class
and both implementations as subclasses. It retains the sole class upper bound
and compares virtual dispatch with the same identities and failure ordering.
