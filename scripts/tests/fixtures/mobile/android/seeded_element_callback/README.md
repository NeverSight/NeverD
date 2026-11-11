# Seeded callback with an Object-erased formal result

Independent Java declarations pass a direct seed and a three-slot callback whose
inputs include that formal and a closed nominal context, and return the same
formal. A public static member forwards to a second provider without changing
any callback, cast or result. Frozen callers exercise ordinary calls, reflection
and method handles with two ordered callbacks, arbitrary/null results, provider
overrides, partial writes and exact failure identity. A deliberately wrong result
fails only at the original observer cast after both callbacks. All original
metadata, including generic signatures and member scope, is compared. Both owned
and imported provider declarations are covered; no application data is used.
