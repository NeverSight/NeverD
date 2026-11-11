# Static return-only overloads

Owned Java bytecode corpus for two independent static generic methods with identical
parameter descriptors and distinct Object / Box return descriptors. javac compiles
the two Java-legal names first. Before freezing or execution, the runner changes
only those UTF8 method-name constant-pool entries to `select` in Core and Client;
all Code attributes and non-name metadata remain byte-identical. This creates a
legal JVM/DEX form Java cannot declare directly. It includes no external code.

Frozen observers cover direct recovered callers, a separate caller, reflection and
MethodHandles, both result slots, delayed payload reads, null/wrong results, wide
arguments, exact exceptions, initialization and sticky initialization failure.
Source alias/ABI/reflection differences must be disclosed, every original body
retained and every generated source compiled. Independent wrong programs must be
detected by the same observer.

The `--synthetic-slots` profile sets only ACC_SYNTHETIC on both owned return
slots before freezing. Every Code attribute and all other bytes stay identical.
The report must disclose the lost synthetic flag for both the renamed entry and
the entry retaining its name. Generic metadata, Exceptions and all remaining
access flags stay exact. The same immutable runtime observer and wrong programs
apply to both profiles.
