# Field reads during constructor argument evaluation

These owned Java and Kotlin sources retain an ordered volatile field read before
each provider callback in a 24-argument constructor. The provider changes the
field after each read, so using a cached or delayed value changes the observed
identities. The field holder initializes inside the argument region; independent
failure cases check its checked/error initialization failures and subsequent
poisoned-class access, separately from the allocated class initialization.

Source and dependency-provider profiles each retain seven original classes or
one input facade respectively. The frozen observer compares 75 fresh JVM cases,
two attempts per case, and rejects twelve wrong programs. All original class,
method, field and Exceptions metadata is audited with the reported source debug
and compiler differences. D8 output is used without binary or Signature patches.

Native model cases separately exercise aliased field destinations for exact
static and instance read forms, all primitive widths and reference/array fields.
They reject uninitialized field receivers, lost aliases, broken wide pairs,
unknown fields, malformed operations and mismatched field declarations.
