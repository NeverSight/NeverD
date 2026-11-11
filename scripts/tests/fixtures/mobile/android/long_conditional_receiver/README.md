# Long conditional constructor arguments

These independently authored Java and Kotlin sources exercise a constructor with
24 reference arguments computed by an inlined helper. Each argument retains its
provider read, mapper lookup and null/Boolean branch. The D8 release build is
used without classfile, DEX or Signature patches.

The frozen observer uses reflection, a method handle and a compiled caller. It
checks allocation class initialization before argument effects, all argument
identities, ordered callbacks, null and Boolean cases, checked exceptions,
errors, repeated calls and a constructor that exposes its partially initialized
object before failing. Source and dependency-provider profiles each run 69 fresh
JVM cases, twice per case, and reject ten wrong programs before recovery.

Native model fixtures separately check destination-only boolean writes over an
uninitialized receiver alias, lost identities, malformed operations and the
bounded forward region at and beyond its instruction limit.
