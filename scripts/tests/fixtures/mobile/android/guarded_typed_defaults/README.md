# Guarded typed allocation defaults

Owned Java 25 subclass computes a long and a conditional reference before an
exact nongeneric superclass call. Four originally unwritten public fields are
encoded final before freezing or execution: generic callback, long, boolean and
reference array. Independent byte receipts prove all Code and other metadata
unchanged. A frozen superclass observer sees allocation defaults, reflectively
mutates each final field, and can throw. The generated source must initialize
before every original preparation effect and preserve all later mutations.
The runner compiles every generated Java file and compares complete metadata,
ConstantValue absence, callback cast timing, failure identity and frozen inputs.
No external application data is included.

The additional linear profile moves the same conditional selection into one
static normalization helper before compiling and freezing. Effects and immutable
observer expectations remain identical; the constructor has no branch before its
superclass call, so both prefix shapes consume the same structural proof.
