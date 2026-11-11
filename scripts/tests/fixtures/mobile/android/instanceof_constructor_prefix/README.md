# Instanceof constructor prefixes

An owned abstract generic host delegates through an `instanceof`/cast/null prefix before `this(...)`. Closed parameters and a final field of the original owner formal keep their original metadata. A concrete subclass exercises direct, conditional and allocating constructor routes. Source and SDK profiles check typed defaults before initialization, argument and field identities, allocation order, null failures and exact exceptions. No private input is included.

The `--linear-prefix` variant compiles and freezes a separate original whose constructor calls a factory, evaluates `instanceof`, calls a virtual selector and delegates without a branch. An independent DEX witness verifies that order. The separately compiled observer checks Boolean values, factory/selector effects, a future override returning a distinct object, final fields and exact failures. Source and SDK recovery compile every emitted Java 25 unit with original definitions and metadata.
