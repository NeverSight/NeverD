# Fixed SDK interface bridges

This owned program separates a nongeneric implementing class from a fixed
interface ancestor. Moving the complete declarations from DEX into an SDK jar
must preserve the original checked bridge and its virtual typed target.

Four frozen invocation paths run 192 fresh JVM cases twice. A special method
handle calls the original superclass bridge on an overriding subclass. Observers
check cast/null ordering, argument and receiver identity, wide values, partial
callback effects, checked and unchecked exception identity, and late reads.
Fifteen independent wrong programs qualify those observers. Eight class Signature
controls change only the fixed ancestor's constant-pool UTF8 entry, preserving
all Code and descriptors. Every emitted source compiles and retains original
class/method definitions, bridge/synthetic roles, Signature and Exceptions.
The report continues to identify the compiler-regenerated bridge as projected.
