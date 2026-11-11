Independently authored Java fixture for a nongeneric source interface with a
static final field and a method-local type parameter. A generic wrapper and
implementation retain separate class and method scopes. The unchanged JVM
harness checks virtual calls, exact returned identities, nulls, ordered field
effects, exception precedence and generic reflection before and after recovery.
Signature-only counterexamples retain the original Code and descriptors.

Reflected generic declarations and type variables use `equals`, independently
of JVM reflection caching, as required by the [TypeVariable contract](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/reflect/TypeVariable.html).
Returned application objects and exceptions retain exact identity checks.
