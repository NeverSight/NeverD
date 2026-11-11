# Invariant token results with closed checked throws

The owned public static provider declares `<T> List<T> read(ByteBuffer, Class<T>)`
and a concrete checked exception. One independent context supplies no type
binding; the invariant token and result use the same method formal.

Source and SDK profiles compile every emitted Java unit and compare original
class/method identities, Code/access, Signatures and Exceptions. Four frozen
callers run 144 fresh JVM cases twice. They observe null and wrong tokens,
opaque and empty results, buffer identity and mutation, callback ordering,
partial writes, exact checked/runtime/Error objects, retained result reads and
later element casts. Eleven independently compiled wrong programs and eight
same-Code Signature controls must fail their independent observers.

`--private-member` adds an owned private static implementation and a public
nested caller. The Java 8 compiler supplies the original package synthetic
accessor, which forwards the erased inputs without a Signature. The same
observers and negative controls cover all paths; source and SDK inventories
include the complete nested definition. Source recovery must disclose exactly
that accessor's synthetic flag loss and retain every other method role,
Signature, Exceptions and class metadata value.
