# Generic superclass arguments during allocation

Owned sources allocate a result wrapper before evaluating a generic superclass
call with reference and wide arguments. The wrapper constructor consumes the
opaque returned object and the original caller. Frozen callers observe class
initialization, exact superclass dispatch, partial construction, argument and
result identities, wide bits and checked/runtime/Error propagation. Separate
future overrides and repeated attempts cover erroneous class initialization.
No third-party names, code or data are included.
