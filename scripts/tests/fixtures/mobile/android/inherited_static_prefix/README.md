# Inherited static fields before constructor delegation

Independently authored Java constructors reference protected mutable fields
through a subclass two superclass edges from the declaration. Original class
initialization, wide/reference values, volatile snapshots, this/super delegation,
partial field writes and callback failures remain observable. Source and SDK
ancestor profiles use frozen callers and observers and compile all recovered Java.

The `--interface-scope` runner variant adds explicit `Cloneable` and `Serializable`
interfaces to the intervening Core and Parent headers before compiling the owned
original. It retains the same frozen callers, runtime cases and wrong programs;
recovered sources are never rewritten.
