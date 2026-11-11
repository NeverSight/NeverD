# Deferred plain callbacks across abstract classes

Parent<T,V> combines a generic Order<T> contract with the plain Updates interface.
Middle<M,N> remains abstract and inherits all pending callbacks. Child<S,V>
provides the original concrete callbacks. The redeclaration profile adds an
original abstract removed(int,int) declaration to Middle before javac compilation.

Source and immutable dependency interfaces exercise both missing and redeclared
intermediate methods. All original method descriptors, access, generic scopes,
field metadata and Exceptions attributes must remain unchanged. Recovery may not
invent intermediate callbacks; reported private throw helpers remain explicit.
Immutable callers exercise child, intermediate class, parent and interface routes,
including parent forwarding effects, nullable payload aliases and ordered failures.

Native controls reject missing concrete implementations, static/protected
providers, incompatible method scopes, conflicting abstract interfaces without a
concrete descendant, and unknown declarations. Original-Code counterexamples and
independent javac controls reject missing or incompatible concrete callbacks.
Each JVM matrix checks 20 values, 462 identities, 147 states and 42 failures.
