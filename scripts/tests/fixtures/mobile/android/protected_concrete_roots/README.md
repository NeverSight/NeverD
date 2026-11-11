# Independently owned concrete virtual roots

Owned Kotlin classes declare an open protected method with a body on an abstract
parent and an unrelated public method with the same JVM name and descriptor.
Known subclasses override both branches. Naming must retain each original root
and its complete override family without borrowing another abstract declaration.
The SDK-provider profile excludes only the legal Java Trace provider.

The frozen observer checks direct and reflected virtual dispatch, four runtime
receiver types, two receiver identities, full-width long values and both root and
override fields. It checks supplied runtime exceptions, errors and checked
exceptions, null throws and receivers, and partial fields before and after the
provider hook. Independently compiled wrong programs are rejected before the
CLI runs. Every emitted Java unit must compile; definition identities, source
aliases, Signatures, Exceptions and metadata are checked with precise receipts
for unavoidable source-name and Kotlin filename differences.
