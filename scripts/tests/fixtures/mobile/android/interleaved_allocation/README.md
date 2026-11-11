# Interleaved allocation control flow

Owned Java defines switch branches returning null or a newly constructed target.
The independent ASM input spells out the same operations with a sibling branch
physically between one allocation and its conditional argument tail. Both Java
reference and original bytecode run against frozen callers before D8. The runner
requires D8 to retain that interleaving, then checks all recovered Java, entries,
metadata, initialization order, opaque effects, partial final stores and failures.
No third-party sample code or metadata is used.
