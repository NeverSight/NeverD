# Mixed raw callback and Unit bridges

The authored Java bridge keeps a raw generic callback interface, a Void cast,
a Context cast, the original Number.intValue call and the original typed void
target. Unit is read only after that target returns. The runner marks only the
explicit method's bridge/synthetic access bits; its compiler Code stays intact.

Six frozen call routes run 108 fresh JVM cases with two attempts. They observe
cast precedence, a custom Number's intValue side effects/failures, null values
and receivers, Long/Double/Float/Byte conversions, exact captured receiver and
references, primitive bits, partial writes, and first/repeated Unit initializer
failures. The typed route also checks argument evaluation before a null target
receiver. Legal wrong programs test casts, unboxing order, receiver, target and
singleton behavior. Every generated Java file compiles; original definitions,
raw ancestry, descriptors, metadata and Exceptions must remain, except the
reported bridge/synthetic flags and reported throw helpers.
