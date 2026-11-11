# Stateless anonymous method contracts

A genuine Kotlin field-initializer object implements a nongeneric Java interface
with a method-owned generic declaration and a closed parameterized declaration.
The original anonymous class has no fields or class Signature and only an Object
constructor. The fixture changes only the two implementation bridge flags;
original compiler Code and all Signatures remain unchanged.

The frozen Java caller and observer cover five dispatch routes, nullable inputs,
mutable aliases, callback writes, repeated calls, and exact exception identity
and order. Full class/method inventories and metadata are checked after the
reported anonymous name, annotation-retention and bridge-flag differences.
Same-erasure changes to an unused bound, result formal and fixed arguments must
fail; independently compiled wrong bodies must fail the unchanged observer.
Both ordinary/synthetic bridge flags and source/dependency interface contracts
are exercised. These are first-party fixtures with no external application data.
