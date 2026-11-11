# Original checked bridges with method-owned scopes

This independently authored interface owns a key formal. Its method owns a
separate recursive `M extends Enum<M>` bound and invariant `Class<M>` parameter.
The implementation fixes the key to String and retains an alpha-equivalent
method scope. Javac supplies an original erased bridge that checks the key,
invokes the typed method virtually and returns its unchanged Enum erasure.

Source and immutable interface-JAR profiles require complete original compiler
metadata, every runtime entry and one reported regenerated bridge. An immutable
JVM harness checks direct, interface and reflected calls, an independent subclass
override, enum identities, mutable trace fields, null receivers, key failures
before target dispatch and failures after target effects. Same-Code self-bound
and invariant parameter drift must fail recovery and independent javac. Native
controls also reject missing bridges, wrong casts or argument order, direct
invocation, throws and absent method Signatures.
