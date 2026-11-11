# Shared enclosing formal and original long conversion

The authored Worker is created by two generic factory methods. The runner
changes only its owned class Signature, enclosure metadata and invoke flags
to encode a shared anonymous callback whose S belongs to Owner.make.
Every Code attribute and runtime descriptor stays unchanged.

The independent JVM oracle checks wide values, non-Long Number implementations,
unboxing effects, nullable captured result identity, separate instances and
ordered exception identity. It checks the original exact enclosing formal and
the reported source scope/interface/bridge metadata changes. A target-only
S-to-V Signature mutation must fail even though both erase to Object.
