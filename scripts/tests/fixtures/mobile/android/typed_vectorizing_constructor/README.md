# Typed vectorizing constructor delegation

Original Java signatures declare an Object class formal and a nominally bounded
class formal. An interface method independently binds its receiver formal and
method result formal before one exact same-owner constructor call. Original
conversion, receiver/input identities, partial fields and failures are checked
through frozen callers and implementations. No application data or metadata
edits are used.
