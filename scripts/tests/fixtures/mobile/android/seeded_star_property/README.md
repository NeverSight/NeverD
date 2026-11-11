# Seeded wildcard property fixture

This owned fixture relates a direct method seed to the invariant first slot of
`Token<T, ?>`, with an independent String context and an opaque nominal result.
The abstract contract, concrete implementation and generic callers declare the
same checked exception. No proprietary input or literal is included.

Source and SDK layouts compile every recovered unit and compare all original
class/method metadata, Code roles, Exceptions and exact generated helpers.
Frozen callers and observers run 192 fresh JVM cases twice. They check receiver
factory effects, future overrides, nulls, context identity and aliasing, discarded
results, callback writes and exact exception objects. A separately compiled
property implementation checks its first-slot bridge only when the frozen caller
reads it after the provider has completed; the wildcard result stays independent.
Fourteen wrong programs qualify these observers before recovery. Ten same-Code
Signature controls reject missing scopes, changed erasures/bounds and unproved
property variances. Original classes and observers remain byte-identical.
