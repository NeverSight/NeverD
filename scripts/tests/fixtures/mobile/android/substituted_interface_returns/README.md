Independently authored typed iterator interface. A bounded pre-baseline class
edit removes only its original javac Object-return bridge while preserving all
other Code, descriptors and metadata. The concrete implementation retains both
original runtime slots, so narrow and erased calls have an independent baseline.

Recovery preserves all original declarations and reports the additional bridge
that javac regenerates from the full Iterator<Item> contract. The oracle checks
complete inventories/metadata, nulls, original reference and exception identity,
mutable calls and bridge/default reflection. A Signature-only different result
argument is rejected; it cannot authorize a generated slot with the wrong type.
