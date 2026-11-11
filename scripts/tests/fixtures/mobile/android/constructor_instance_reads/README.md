Owned original Java8 copy constructors read private instance fields and public
SDK-declared fields before a same-owner constructor. The getter mutates source
values and caller globals; the delegated constructor replaces them again.
Frozen four-route observers check source/argument snapshots, every primitive
width, raw NaN and negative-zero bits, reference/array identity, null receivers,
partially initialized final fields and original runtime/error identities.
No input Code, flags or metadata is repaired to fit recovery.
