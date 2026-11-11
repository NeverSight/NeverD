# Retained reference from constructor preparation

Original Java 17 source uses a switch expression in the first constructor
argument to retain a reference independently of four repeated float arguments.
D8 emits the call result before the constructor and keeps it live afterwards.
Frozen callers observe class initialization before the call, original raw float
bits, reference identity, ordered exceptions and partial final fields. The
runner compiles every recovered source, checks complete entries and metadata,
and rejects independently compiled wrong programs.
