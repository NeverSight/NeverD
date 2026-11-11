# Array arguments allocated before constructor delegation

Owned Java sources allocate an argument object, create its reference array, initialize that object and delegate to another constructor. Release D8 output has no branch between those allocations. No classfile or DEX patching is used.

The frozen observer compares reflection, method handles and an independently compiled constructor call. It checks class initialization before negative array failure, poisoned classes, fresh array identity, callback mutations, checked and error exception identity, partial effects and repeated construction. Deliberately wrong programs must fail before recovery. Source and dependency-provider profiles retain original definitions and metadata; the recovered prologue requires Java 25.
