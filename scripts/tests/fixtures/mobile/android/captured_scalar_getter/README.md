# Result-only method formals on proven captured members

An owned nongeneric member captures its original outer object and implements a
nongeneric interface with an independent `<T> T get(int)` contract. Frozen
callers exercise concrete and interface dispatch, an original compiled subclass,
null receivers, mutable outer arrays/indices, raw return identity, original
later casts, Object overload selection, and exact exception/error identity.

Source and borrowed-interface profiles retain the owned constructor capture
proof. Java 25 source discloses only the existing static-member/capture-field
metadata projection; full method Signatures and Exceptions stay unchanged.
Every recovered definition is independently inventoried and compiled. Signature
mutations and separately compiled wrong implementations must fail the immutable
observers. No private application code is executed.
