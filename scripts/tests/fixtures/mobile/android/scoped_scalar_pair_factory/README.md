# Scalar factories with independently scoped pair results

A generic class defines independent same-name static method formals and returns
its own invariant pair type from boolean and mixed integer/boolean factories.
The scalar context supplies no type binding. Mutable state owns the actual
result, callback effects and failures; neither allocation nor stored contents
are inferred from the complete returned type.

Source and declaration-only profiles preserve every original compiler entry
and metadata contract, compile every Java source, and compare frozen direct,
compiled caller, reflection and method-handle observers. The observer checks
configuration snapshots, callback timing, opaque/null/replaced results, late
map operations and casts. Wrong programs and Code-preserving Signature
controls run before accepting recovery.
