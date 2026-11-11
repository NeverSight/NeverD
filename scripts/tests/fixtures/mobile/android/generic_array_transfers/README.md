# Generic array transfers

The independently compiled provider performs an actual arraycopy and returns
the original destination. The element-seed profile also accepts the same
Object-bounded method formal as a scalar and stores that exact value after the
copy. Both Java sources and immutable observers are authored before compilation;
original classfiles and D8 output are never rewritten.

Direct, separate caller, reflection and method handle routes compare source,
destination, seed and returned reference identities, class initialization,
ordered provider effects, controlled exception identity, nulls and array
bounds. The element profile additionally checks null elements, a covariant
element-store failure after a successful copy, and a self-referential overlap.
Every generated Java file compiles at release 8. All original definitions,
class/method metadata and Exceptions are retained in source and SDK profiles.
Seven independently wrong element-profile programs must fail the frozen oracle.
