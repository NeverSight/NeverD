# Scoped comparator in an erased generic allocation initializer

An owned generic child forwards its exact incoming Comparator<? super K> to
the original generic map initializer beside a Map overload, then supplies the
initialized raw map to the covariant generic parent constructor. The allocation
has an unused value formal; no value binding, map contents or comparator result
is inferred. Four frozen constructor routes compare 96 cases twice with source
and SDK allocations/parents, checking exact stored comparator identity, null
comparison policy, actual compare calls, argument order, partial writes, exact
failures and later key casts. Every recovered source unit and original metadata
entry is compiled and audited, including a methodless marker. Wrong programs
and Code-preserving Signature controls guard observation and scope. Only owned
maps and comparators execute.
