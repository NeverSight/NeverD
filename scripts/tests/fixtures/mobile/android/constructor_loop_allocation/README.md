# Allocation in constructor argument loops

Original first-party Java 25 source evaluates constructor arguments through
zero or repeated loops with a fresh Token allocation before its effectful
arguments. Frozen callers observe reference identity, complete long words,
ordered failures and partially initialized objects. The runner also checks
original class and method metadata and compiles all recovered Java.
