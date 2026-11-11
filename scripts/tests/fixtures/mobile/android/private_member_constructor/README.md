# Private member constructor contracts

Owned private static members delegate through complete closed collection
constructor contracts. The original callback array, null checks, generic list
factory, collection copy, integer input, Throwable identity and constructor
continuations remain explicit. The enclosing class and exact private static
member boundary supply no generic variable or inferred behavior.

Fresh JVMs compare callback/input identities, collection snapshots, partial
construction, callback order and exact failures. A public-member control shares
the same bodies. Both profiles compare every original compiler fact and runtime
definition, without source ownership or signature projections. Wrong programs
must fail the frozen observer before recovery. Only owned code is executed.
