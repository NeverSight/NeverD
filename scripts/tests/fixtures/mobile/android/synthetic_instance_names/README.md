# Package synthetic instance identifiers

This independently authored Java fixture lets D8 create a package-visible
synthetic instance method and its original lambda adapter. Frozen callers check
captured receiver identity, subclass dispatch, callback order, cleanup, and exact
exception identity. Five independently changed programs must fail the observer.

The `--closed-owner` profile retains `Scope<Runnable>` on the declaring class.
It has no class formals or generic method signature. The same virtual-family
name receipt must preserve that full header, all original entries and metadata,
and disclose the method-name and synthetic-flag changes. Both source and SDK
provider profiles compile all generated Java and run the frozen callers.

The `--generic-owner` profile retains two independent class formals and
`Scope<V>`. The package-synthetic method itself has no generic Signature. Both
profiles run 80 fresh JVM cases twice, retain the original D8 adapter and
captured receiver, and compare all class and method metadata against the exact
disclosed name/flag changes. All five wrong programs are checked before recovery.
