# Closed wildcard superclass constructor input

An independently authored named static member forwards `Carrier<? extends
Child>` into its superclass constructor's `Carrier<? extends Base>`. The known
unary carrier declares `T extends Base`. All runtime descriptors and complete
class, field and method metadata must survive recovery and Java 8 compilation.

The frozen caller observes the actual input, callback count, partially built
child, field order, result identity, original exception and Error objects, and
later checked reads. It also subclasses the recovered child outside the input.
Source and dependency modes each exercise 64 fresh JVM cases twice. Signature
controls retain all original Code bytes; independently compiled wrong programs
must fail against the same frozen caller. Only authored fixture code executes.
