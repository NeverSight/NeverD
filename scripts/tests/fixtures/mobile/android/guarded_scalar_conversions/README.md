# Guarded scalar constructor conversions

Independently authored constructors select an int-to-float or double-to-long
conversion before a same-owner private delegation. Frozen direct/reflection
callers observe selected reads, lazy provider initialization, exact failures,
partial final fields, rounded/clamped values and raw fallback float bits.
No application data is included.
