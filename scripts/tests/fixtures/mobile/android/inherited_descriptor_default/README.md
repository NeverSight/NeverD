# Inherited descriptor default accessor

Kotlin 2.0.21 compiles Parent with `-Xjvm-default=all`, then Child with
`-Xjvm-default=all-compatibility`. The unmodified child bytecode invokes its
inherited default through a self-owned symbolic reference. Parent has no
static compatibility accessor. All original Code bytes remain compiler output.

An immutable Java observer compares ordinary virtual dispatch, DefaultImpls,
reflection and MethodHandle access to the original synthetic static entry.
It checks receiver/input/result identity, null receivers, failures and partial
effects, and distinguishes overriding receivers from the inherited default.
Source and SDK profiles keep the default interface owned; the latter supplies
the trace implementation as an independent declaration JAR.
