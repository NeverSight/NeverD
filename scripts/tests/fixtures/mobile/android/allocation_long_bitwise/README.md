# Long bitwise allocation arguments

Independently authored JVM sources evaluate six total long bitwise operations
between two original allocations. Long operands and int shift distances come
from observable providers. The immutable observer checks high-bit values,
negative and large shift distances, exact provider/constructor order, partially
published final fields and original failures. No application data is used.
