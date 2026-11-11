# Sequential monitor scopes

The Java/D8/JVM profiles retain two independent synchronized regions, callbacks
between them, early returns, null acquisitions, identical lock objects and
caller-held locks. Four frozen call routes check exact acquisition/release and
result/failure identity at every event, including two attempts per fresh JVM.

`--outer-cleanup` authors a surrounding Java `try/finally` before compilation.
The observer requires cleanup after both acquisitions/releases or either failed
acquisition; pre-existing caller-held locks remain held. It checks normal and
exceptional early exits, RuntimeException and Error cleanup replacements,
unchanged definitions/metadata/Exceptions and every recovered Java compilation.
Eight independent wrong programs test the oracle. `--loop` independently checks
released continuations returning to the first acquisition.

`--covered-capture` uses independently authored smali for the exact layout
where an outer range includes an inner exception capture before release. The
same frozen Java specification and observer check behavior; only Core is owned,
with the original Trace class as a dependency. This input is explicitly authored
smali rather than compiler output. Protected captures, copies and constants
must not introduce exception edges; actual calls retain their original cleanup.

`--lock-aliases` uses the same frozen outer-cleanup specification and observer
with authored `AliasCore.smali`. Each lock is copied after acquisition, then its
original register is reused by a wide write (once as the high word). Normal and
exceptional releases use the retained object aliases. The observer still checks
both sequential scopes, caller-held locks, null acquisition, exact failures and
all generated Java; this smali is never labeled compiler output.
