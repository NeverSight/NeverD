# Checked star carrier superclass constructor

The Java 25 specification stores two fields before calling the exact Key<?>
superclass overload. The carrier retains its original interface bound. The
parent invokes virtual field readers while constructing, so late or lost
captures are observable. Authored smali reuses an incoming capture slot only
after saving the field, preserves the original check-cast, and never claims to
be javac/D8 output. All other owned classes form the frozen SDK dependency.
The fixture changes only the two-byte class version for the declaration reader
and checks identical metadata/instructions plus the same frozen JVM observer on
both the Java 25 originals and compatibility encoding before recovery.

The observer compares actual references, nulls, cast failure, exact constructor
and initialization failures, failed initialization caching and partial effects
through direct, caller, reflection and MethodHandle routes.
