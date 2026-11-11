# Discarded reference results beside a covariant return bridge

Owned Java specifies a fluent reference result, an interface bridge and a final
void adapter which calls the fluent method once. The fixture encoder changes
only the adapter's constant-pool name and synthetic flag, and its owned caller's
constant-pool reference, to express the legal JVM return-only overload. Every
compiler Code attribute stays byte-identical; this metadata encoding is reported
explicitly rather than described as unchanged compiler output.

Frozen observers compare the original encoded classes and recovered source via
an owned caller, reflection, MethodHandles and interface dispatch. They inspect
null receivers, char code units, actual returned identities, callback ordering,
partial writes, failures and adapter stack frames. All definitions and complete
metadata are compared except the disclosed void source name/synthetic flag and
reported compiler helpers. Input bytecode and observers remain frozen.
