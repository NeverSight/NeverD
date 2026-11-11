# Throwing long allocation preparation

Independently authored Java fixture for signed long division and remainder
inside nested object allocations. The external immutable harness uses BigInteger
as its value oracle, preserves class initialization and callback order, checks
partial fields and original exception identity, and runs two attempts per case.
Both source and SDK-provider profiles compile every recovered Java unit and
compare all original entries and metadata. Wrong-program controls include lost
unused divide/remainder effects and replaced or swallowed exceptions.
