# Projected interface with unchanged superclass delegation

The original constructor passes an integer literal and the same invariant Sink
reference to a plain parent. A separate, proven value carrier bridge requires
one raw source interface projection. Its unrelated superclass and full
constructor Signature remain unchanged. Frozen callers observe original object
publication, virtual dispatch before parent field initialization, wide values,
nulls and exact exception identity. Four independently compiled wrong programs
must fail. Only the recorded interface Signature and original bridge flags
change in source; all original entries, fields and other metadata are audited.
