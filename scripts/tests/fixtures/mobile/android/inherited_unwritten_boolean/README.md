# Inherited unwritten Boolean final

An abstract Owner extends Base. Base's constructor invokes Owner's final virtual
callback before Owner initialization. A frozen observer reads the zero allocation
default and can reflectively mutate that field before superclass construction
finishes. Both primary and delegating constructors must preserve that mutation.
The observer also checks the exact exception object at each construction stage,
partial final fields, repeated constructions, and reads after construction.

Original Java 8 constructors never store zeroFlag. A witnessed access-flag-only
class edit adds final while preserving Code, descriptors, signatures, and every
other attribute. The recovered field must retain its absent ConstantValue and
original metadata. All input code is independently authored; no application data
is used. Independent wrong programs reset the flag after super, return a constant,
skip the superclass callback, or truncate the long final field.

The runner's `--package-owner` variant copies these authored sources into its
fresh work directory, makes Owner package-private and zeroFlag private, and
compiles frozen same-package callers. Reflection still observes and mutates the
actual private field before and after superclass construction. Only the witnessed
final flag changes between the original and encoded classes; all other metadata
and Code remain exact. Both owned and dependency observer variants recover and
compile every generated source with Java 25 constructor prologues.
