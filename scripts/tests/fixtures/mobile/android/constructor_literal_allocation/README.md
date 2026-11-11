# Independent constructor literal allocation

An auxiliary constructor allocates Token with three independently loaded null
arguments and one signed integer literal before delegating to its private
same-owner primary constructor. Original input identity, a wide word and label
are preserved across the literal register writes and delegation. The mask is
3 or -7; both source and SDK Token declarations are covered.

Frozen Java 8 callers observe exact Token class initialization, fresh object
identity, partial final fields, null and raw wide inputs, callback failures and
sticky initialization failure through four routes with two attempts per case.
Every recovered source compiles at its reported Java release, and inventory,
metadata and Exceptions are checked. Legal wrong programs alter literal slots,
mask, input identity, wide bits or final-store timing.
