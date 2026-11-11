# Closed callback constructor regression

An owned static member constructs a private concrete `Supplier<String>` from two incoming references before invoking a generic superclass constructor. The complete declared interface supplies its closed type argument. The callback result is an opaque snapshot, distinct from the text argument and the context field after the parent mutates it.

The immutable JVM observer checks constructor overload selection, original reference identities, class initialization order, partial construction, callback effects, nulls, and exact failure identity over repeated attempts. Source and dependency declaration profiles preserve compiler metadata and the original definition inventory. Independently compiled wrong programs must fail the observer.

The sole source-projected method is the original covariant `Supplier.get()` bridge. The runner requires that exact bridge receipt, independently compares its regenerated compiler identity and metadata, and exercises it through the parent interface call. Every other original method must have a recovered body.
