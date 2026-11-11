The Java fixture declares its original raw generic interface explicitly and
retains a separate primitive target with a complete contravariant tail Signature.
Only the exact erased entry's bridge/synthetic flags are encoded; Code and
descriptors stay unchanged. Boolean and long profiles check input casts,
effectful Number dispatch, null and failure order, continuation identity,
unchanged returned markers and a frozen direct primitive caller. API and tail
declarations can be frozen in a dependency JAR.
