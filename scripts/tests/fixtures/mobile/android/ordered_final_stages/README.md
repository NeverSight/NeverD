# Ordered final initialization stages

Owned Java fixture for alternative final stores inside a protected constructor body and its original finally cleanup. Recovered assignments must remain observable at the same callback boundary, including when a checked exception or Error escapes or cleanup replaces an earlier failure.

The runtime observer is compiled independently against the original Java 8 classes. It exercises reflection, method handles and a frozen direct caller, two attempts per fresh JVM, both branches, wide arguments, null and aliased values, and partially constructed escaped objects. Source and dependency-provider profiles compile all recovered Java and compare complete definition and declaration inventories. No input class or DEX patching is used.
