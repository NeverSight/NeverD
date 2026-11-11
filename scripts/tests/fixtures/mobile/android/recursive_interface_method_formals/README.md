Independently authored generic method contract with a single recursive interface
bound, `R extends Bound<? super R>`. The unchanged external contract uses an
alpha-equivalent formal. JVM checks preserve full bound reflection, the class
and method scopes, raw reference identities, subclass dispatch and exception
order. Signature-only invariant-versus-contravariant bound drift leaves Code
and descriptors unchanged and must fail both NeverD and independent javac.
