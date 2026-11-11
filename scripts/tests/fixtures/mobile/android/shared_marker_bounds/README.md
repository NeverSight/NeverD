# Shared method marker bound fixture

These independently authored classes model one captured method formal whose
sole bound is `Flag<? extends Value>`. `Flag<R>` is a known, empty, direct marker
interface with one Object-bounded formal. The closed argument has no type variables.

The shared upper bound runner's marker profile changes only Worker class Signature
and enclosure metadata; it compares every original Code attribute and descriptor.
The original method formal and its complete wildcard bound remain intact. Shared
field, constructor and result uses retain that full parameterized bound in source.

The D8/JVM harness compares two concrete implementations, mutable captures, one
class identity across factories and copying, 34 returned identities and ten ordered
failures. It checks the retained wildcard in every projected use, the recorded
reflection changes, and the expected original phantom self reflection failure.
No private input or derived source is included.
