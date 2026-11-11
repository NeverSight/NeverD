# Encoded static defaults before constructor delegation

Unmodified javac output and genuine D8 encoding retain runtime final fields
before a later encoded constant. D8 explicitly writes their typed preparation
zero into the static values array; the original class initializer then assigns
the real values. Guarded constructor delegation must retain runtime reads.

Source and SDK-callback profiles compile every generated Java unit, compare all
definitions and metadata/Exceptions, and check fresh virtual-machine behavior
through compiled callers, reflection, exact MethodHandle and no-argument entry.
The observer checks initialization order, reentrant preparation defaults, every
mask combination, wide/float raw bits, token identity and partial constructor
failures. Independently compiled wrong programs must be detected.
