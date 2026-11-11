# Conditional argument programs and allocation receiver aliases

An unmodified Kotlin compiler and D8 release build produce a forward argument
program and a long contiguous constructor frame. The original allocation and
its receiver copy must remain one identity. Native cases also cover aliases
saved on separate branches, a reused original word, and restoration at the join.
No compiler output or Signature is patched.

The immutable JVM observer checks class initialization before argument effects,
ordered reads and callbacks, nulls, reference identity, mutable payloads, exact
checked failures and Errors, partially initialized objects, and repeated calls.
All emitted Java is compiled. Metadata differences are limited to reported
helpers, Kotlin source filenames and CLASS annotations, and the Java release
needed by the argument switch expression.
