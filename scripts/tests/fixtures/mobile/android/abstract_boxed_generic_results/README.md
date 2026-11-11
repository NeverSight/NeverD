# Abstract boxed generic results

This authored fixture keeps an abstract scalar getter on `Core`, an effectful
`Child` override, and two original five-operation getter/factory return entries.
The runner changes only the original interface Signature, Object bridge flags,
and the final typed entry name from `typedNext` to `next`; encoding receipts
prove unchanged Code and descriptors. D8 witnesses both original full bodies.

The immutable harness compares 246 values, 804 identities, 636 states and 42
failures across eight direct/interface/reflection/MethodHandle routes and two
inherited default callback routes. It checks wide boundaries, mutable receiver
and factory aliases, new/cached/null results, exception identity and call order.
The original abstract getter and concrete subclass override keep virtual
semantics. The final typed entry uses its reported source name after recovery.

Source and immutable interface-JAR profiles compare every original class,
field and method record, permitting only the reported Object interface argument,
Object bridge flags, final typed source name and private helpers. Same-Code
argument drift, missing synthetic flags and missing typed final flags fail.
An independently compiled conflicting boxed result also fails javac.
No typed entry, boxing operation or scalar body is synthesized.
