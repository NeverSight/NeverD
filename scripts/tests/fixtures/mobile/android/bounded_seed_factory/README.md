# Bounded seed factories

Independently authored Java contracts share one interface-bounded method formal
between a List seed, a contravariant fixed-input producer and the List result.
Frozen callers check original effects, bound casts, wide data, partial mutation,
exception identity and the actual returned reference, which can differ from the
seed or be null. Reflection checks the complete original Signature. No private
application data is included.
