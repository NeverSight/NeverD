# Package-visible generic covariance

A nonpublic `Parent<P>` owns a package getter returning `Result<P>` and original
observable effects. Public `Core extends Parent<String>` overrides it with
`Leaf<? extends Tag>`, whose class declaration implements `Result<String>`.
Its original public `Result` return bridge delegates to the typed getter.

The immutable harness compares 56 values, 322 identities, 168 states and
28 failures across seven direct, typed/raw parent, reflected and MethodHandle
routes and an independent leaf override. Mutable aliases, result identity,
ancestor exception objects and null receivers remain matched. Source and
immutable Parent-JAR profiles retain five classes/seven methods and four
classes/five methods, full original metadata and all runtime entries apart
from reported private helpers. Coverage is partial only for one original
compiler bridge, without generic metadata or class-source projections.

Original D8 return-bridge opcode witnesses are checked. A same-Code leaf
interface argument change rejects in NeverD and independent javac. Native
controls compare full parameters/results and original public/package bridges,
reject private/final drift and changed forwarding, and require visibility over
every intervening class. Default-package access bridges use the same shared
package helper. Input, harness, SDK and parent JAR bytes remain immutable.
