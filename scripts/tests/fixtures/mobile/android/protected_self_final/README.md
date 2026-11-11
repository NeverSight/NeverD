# Protected final stores on this

Untouched Java 8 compiler originals initialize two blank final fields inside a
try/finally, with an earlier final store and a later final store. The catch-all
captures one exception, calls its original void cleanup and rethrows the same
register. D8 must preserve both protected stores and the trailing abrupt handler.
Narrow/wide results and source/SDK providers are covered.

Frozen callers run before recovery. Fresh JVMs observe the published partial
object at every getter and cleanup, including allocation defaults, raw long
words, changing getter sources, constructor and cleanup failures, replacement
exceptions, Error cleanup and sticky class initialization. Every generated Java
source, original entry, metadata and Exceptions table is independently checked.
Eight wrong programs must fail the same observer. Native tests retain exact
this aliases and reject skipped/repeated stores or changed cleanup boundaries.
