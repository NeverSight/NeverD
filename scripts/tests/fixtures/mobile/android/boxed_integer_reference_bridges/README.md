# Integer conversion with an original reference-returning target

These independently authored classes use the same exact Number.intValue
conversion as the integer/Unit fixture. The bridge returns the primitive target's
original Integer result; Integer.valueOf remains inside that target. Only the
class Signature and bridge/synthetic flags are edited after javac.

The JVM oracle compares forty values, eight identities and eight ordered failures,
including integer edges, the required small-value cache, other Number classes and
an authored Number override with effects and exceptions. Changing only the
implemented interface input to Boolean, with identical Code and descriptors,
must fail. The tracing class is reused from `value_carrier_bridges`.
