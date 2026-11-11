# Array views before constructor delegation

Independently authored fixed-array varargs constructors retain a closed generic
parent and nested covariant list/array parameters. Frozen callers check the
original constructor flags and signatures, the selected overload, backing-array
sharing, fixed-size and array-store behavior, null-array failure timing, opaque
list inputs, default-field capture and partial final fields on exact callback
failures. Generated Java must compile with every original definition and helper
role accounted for. No private application code is part of this fixture.
