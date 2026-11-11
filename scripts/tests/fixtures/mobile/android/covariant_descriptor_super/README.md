# Closed descriptor upcast into a covariant superclass input

An authored child accepts `Leaf`, whose complete original superclass is
`Carrier<Payload>`, and forwards it to `Builder<Payload>` through
`Carrier<? extends Payload>`. Both generic declarations retain their known
nominal class bounds. An effectful null check and local copy precede `super`.
Native cases additionally retain the original redundant upcast instruction.

The fixture uses Java 25 constructor prefix statements. A separate D8 input
copy changes only its class-file major version from 69 to 52; all Code and
metadata bytes stay identical and both copies run against one frozen caller.
Recovered sources compile at their disclosed Java 25 level. Every original
identity, signature, field, method, Exceptions and helper role is checked.

Source and SDK profiles each run 64 fresh JVM cases twice, checking prefix
effects before failure, partial objects, exact integer inputs, callback and
Error identity, result mutation, late checked reads and child completion.
Eight Signature-only controls retain Code. Eight independently compiled wrong
programs must fail against the frozen caller. Only authored fixture code runs.
