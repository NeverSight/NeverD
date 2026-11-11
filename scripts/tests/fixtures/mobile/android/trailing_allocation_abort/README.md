# Nested allocation with terminal abort paths

Unmodified Java14 compiler output nests two constructors around a switch
expression, an instance test and an accessor. Failing arms construct and throw
an exception while reusing registers that held abandoned uninitialized objects.
Native variants place abort tails before and after the successful initializer.

Frozen observers check outer/inner allocation and class-initialization order,
getter/style snapshots, raw float bits, result/partial-publication identities,
failure messages and exact runtime/error propagation. Source/SDK profiles
compile all emitted Java and independently account for original definitions,
metadata and Exceptions. Same-Code controls keep ownership failures distinct
from behaviorally wrong programs.
