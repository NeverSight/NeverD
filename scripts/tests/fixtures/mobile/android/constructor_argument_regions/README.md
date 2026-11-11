# Conditional allocations before constructor delegation

These owned Java sources construct two argument objects before delegating to a
private constructor. Nullable interface getters run inside the argument
allocations. A second entry allocates another instance of the same class using
the same constructor descriptor before the actual delegation. D8 release output
also reuses an incoming primitive word after its earlier value has been used.
No classfile, DEX or Signature patch is applied.

The immutable observer uses reflection, method handles and a compiled caller.
It compares 114 fresh JVM cases, twice per case, including class initialization,
nullable getters, wide values, identity/array aliasing, checked and error
failures, partially written objects, poisoned classes and repeated calls. Ten
wrong programs must fail before recovery. Source and dependency-provider runs
retain all original definitions and audit the complete metadata and Exceptions
inventory. The recovered prologue requires Java 25; compiler version changes
and generated throw helpers are reported explicitly.
