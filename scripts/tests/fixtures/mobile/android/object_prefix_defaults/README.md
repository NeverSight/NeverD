# Object constructor prefixes and allocation defaults

A package final owner has a closed contravariant callback constructor parameter,
a linear static guard before Object preparation, one original callback store,
and three unwritten instance slots. The fixture encodes only those slots' final
access bits; all original Code, Signatures and other attributes stay unchanged.

The recovered source must retain prefix effects and exact exceptions, materialize
only proven allocation defaults without ConstantValue, and keep actual field reads
after reflective mutation. Frozen callers, reflection, method handles, reentry and
partial construction exercise source and external-guard layouts. Every emitted
Java unit and original class/method/field identity is checked.
