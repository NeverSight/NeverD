# Nongeneric interface roots of a generic source parent

These independently authored interfaces have no class formals. Their members
include an Object-bounded method formal and a complete parameterized Box result.
A generic abstract parent supplies the original final reference getter and
inherits the unique default; its generic child implements a separate class-formal
method. Box and Trace reuse other independently authored fixture classes.

The JVM oracle retains seven classes and ten methods, compares 27 identities and
six ordered failures, checks class/default dispatch, exact generic reflection and
method ownership, and rejects a changed method-formal Signature with identical
Code and runtime descriptors. Raw uses of interfaces that declare class formals
remain unproven. Generated private helpers and the required Java 9 release are
reported.
